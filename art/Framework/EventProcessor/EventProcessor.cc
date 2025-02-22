#include "art/Framework/EventProcessor/EventProcessor.h"
// vim: set sw=2 expandtab :

#include "art/Framework/Core/Breakpoints.h"
#include "art/Framework/Core/FileBlock.h"
#include "art/Framework/Core/InputSource.h"
#include "art/Framework/Core/InputSourceDescription.h"
#include "art/Framework/Core/InputSourceFactory.h"
#include "art/Framework/Core/InputSourceMutex.h"
#include "art/Framework/Core/ReplicatedProducer.h"
#include "art/Framework/EventProcessor/detail/writeSummary.h"
#include "art/Framework/Principal/ClosedRangeSetHandler.h"
#include "art/Framework/Principal/ConsumesInfo.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/EventPrincipal.h"
#include "art/Framework/Principal/RangeSetHandler.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Principal/RunPrincipal.h"
#include "art/Framework/Principal/SubRun.h"
#include "art/Framework/Principal/SubRunPrincipal.h"
#include "art/Framework/Services/Optional/RandomNumberGenerator.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Framework/Services/Registry/ServiceRegistry.h"
#include "art/Framework/Services/Registry/ServicesManager.h"
#include "art/Framework/Services/System/FileCatalogMetadata.h"
#include "art/Framework/Services/System/FloatingPointControl.h"
#include "art/Framework/Services/System/TriggerNamesService.h"
#include "art/Persistency/Provenance/ProcessConfigurationRegistry.h"
#include "art/Persistency/Provenance/ProcessHistoryRegistry.h"
#include "art/Utilities/Globals.h"
#include "art/Utilities/ScheduleID.h"
#include "art/Utilities/SharedResource.h"
#include "art/Utilities/TaskDebugMacros.h"
#include "art/Utilities/Transition.h"
#include "art/Utilities/UnixSignalHandlers.h"
#include "art/Version/GetReleaseVersion.h"
#include "canvas/Persistency/Provenance/ParentageRegistry.h"
#include "canvas/Persistency/Provenance/ProcessConfiguration.h"
#include "canvas/Utilities/DebugMacros.h"
#include "canvas/Utilities/Exception.h"
#include "cetlib/bold_fontify.h"
#include "cetlib/container_algorithms.h"
#include "cetlib/trim.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/types/detail/validationException.h"
#include "hep_concurrency/WaitingTask.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include <cassert>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace hep::concurrency;
using namespace std;
using namespace string_literals;
using fhicl::ParameterSet;

namespace art {

  namespace {
    ServicesManager*
    create_services_manager(ParameterSet services_pset,
                            ActivityRegistry& actReg,
                            detail::SharedResources& resources)
    {
      auto const fpcPSet =
        services_pset.get<ParameterSet>("FloatingPointControl", {});
      services_pset.erase("FloatingPointControl");
      services_pset.erase("message");
      services_pset.erase("scheduler");
      auto mgr = new ServicesManager{move(services_pset), actReg, resources};
      mgr->addSystemService<FloatingPointControl>(fpcPSet, actReg);
      return mgr;
    }

    auto const invalid_module_context = ModuleContext::invalid();
  }

  EventProcessor::~EventProcessor() = default;

  EventProcessor::EventProcessor(ParameterSet pset,
                                 detail::EnabledModules enabled_modules)
    : scheduler_{pset.get<ParameterSet>("services.scheduler")}
    , scheduleIteration_{scheduler_->num_schedules()}
    , servicesManager_{create_services_manager(
        pset.get<ParameterSet>("services"),
        actReg_,
        sharedResources_)}
    , pathManager_{pset,
                   outputCallbacks_,
                   producedProductDescriptions_,
                   scheduler_->actionTable(),
                   actReg_,
                   std::move(enabled_modules)}
    , handleEmptyRuns_{scheduler_->handleEmptyRuns()}
    , handleEmptySubRuns_{scheduler_->handleEmptySubRuns()}
  {
    auto services_pset = pset.get<ParameterSet>("services");
    auto const scheduler_pset = services_pset.get<ParameterSet>("scheduler");
    {
      // FIXME: Signals and threads require more effort than this!  A
      //        signal is delivered to only one thread, and which
      //        thread is left up to the implementation to decide. To
      //        get control we must block all signals in the main
      //        thread, create a new thread which will handle the
      //        signals we want to handle, unblock the signals in that
      //        thread only, and have it use sigwaitinfo() to suspend
      //        itselt and wait for those signals.
      setupSignals(scheduler_pset.get<bool>("enableSigInt", true));
    }
    ParentageRegistry::instance();
    ProcessConfigurationRegistry::instance();
    ProcessHistoryRegistry::instance();
    // We do this late because the floating point control word, signal
    // masks, etc., are per-thread and inherited from the master
    // thread, so we want to allow system services, user services, and
    // modules to configure these things in their constructors before
    // we let tbb create any threads. This means they cannot use tbb
    // in their constructors, instead they must use the beginJob
    // callout.
    taskGroup_ = scheduler_->global_task_group();
    // Whenever we are ready to enable ROOT's implicit MT, which is
    // equivalent to its use of TBB, the call should be made after our
    // own TBB task manager has been initialized.
    //    ROOT::EnableImplicitMT();
    TDEBUG_FUNC(5) << "nschedules: " << scheduler_->num_schedules()
                   << " nthreads: " << scheduler_->num_threads();

    auto const errorOnMissingConsumes = scheduler_->errorOnMissingConsumes();
    ConsumesInfo::instance()->setRequireConsumes(errorOnMissingConsumes);

    auto const& processName = Globals::instance()->processName();

    // Trigger-names
    servicesManager_->addSystemService<TriggerNamesService>(
      Globals::instance()->triggerPSet(),
      pset.get<ParameterSet>("physics", {}));

    // We have delayed creating the service instances, now actually
    // create them.
    servicesManager_->forceCreation();
    ServiceHandle<FileCatalogMetadata>()->addMetadataString("art.process_name",
                                                            processName);

    // Now that the service module instances have been created we can
    // set the callbacks, set the module description, and register the
    // products for each service module instance.
    ProcessConfiguration const pc{processName, pset.id(), getReleaseVersion()};
    auto const producing_services = servicesManager_->registerProducts(
      producedProductDescriptions_, psSignals_, pc);
    pathManager_->createModulesAndWorkers(
      *taskGroup_, sharedResources_, producing_services);

    ServiceHandle<TriggerNamesService> trigger_names [[maybe_unused]];
    auto const end = Globals::instance()->nschedules();
    for (ScheduleID::size_type i = 0; i != end; ++i) {
      ScheduleID const sid{i};

      // The ordering of the path results in the TriggerPathsInfo (which is used
      // for the TriggerResults object), must be the same as that provided by
      // the TriggerNamesService.
      auto& trigger_paths_info [[maybe_unused]] =
        pathManager_->triggerPathsInfo(sid);
      assert(trigger_names->getTrigPaths() == trigger_paths_info.pathNames());

      schedules_->emplace(std::piecewise_construct,
                          std::forward_as_tuple(sid),
                          std::forward_as_tuple(sid,
                                                pathManager_,
                                                scheduler_->actionTable(),
                                                actReg_,
                                                outputCallbacks_,
                                                *taskGroup_));
    }
    sharedResources_.freeze(taskGroup_->native_group());

    FDEBUG(2) << pset.to_string() << endl;
    // The input source must be created after the end path executor
    // because the end path executor registers a callback that must
    // be invoked after the first input file is opened.
    {
      ParameterSet main_input;
      main_input.put("module_type", "EmptyEvent");
      main_input.put("module_label", "source");
      main_input.put("maxEvents", -1);
      if (!pset.get_if_present("source", main_input)) {
        mf::LogInfo("EventProcessorSourceConfig")
          << "Could not find a source configuration: using default.";
      }
      ModuleDescription const md{
        main_input.id(),
        main_input.get<string>("module_type"),
        main_input.get<string>("module_label"),
        ModuleThreadingType::legacy,
        ProcessConfiguration{processName, pset.id(), getReleaseVersion()}};
      InputSourceDescription isd{md, outputCallbacks_, actReg_};
      try {
        input_.reset(InputSourceFactory::make(main_input, isd).release());
      }
      catch (fhicl::detail::validationException const& e) {
        throw Exception(errors::Configuration)
          << "\n\nModule label: " << cet::bold_fontify(md.moduleLabel())
          << "\nmodule_type : " << cet::bold_fontify(md.moduleName()) << "\n\n"
          << e.what();
      }
      catch (Exception const& x) {
        if (x.categoryCode() == errors::Configuration) {
          throw Exception(errors::Configuration, "FailedInputSource")
            << "Configuration of main input source has failed\n"
            << x;
        }
        throw;
      }
      catch (cet::exception const& x) {
        throw Exception(errors::Configuration, "FailedInputSource")
          << "Configuration of main input source has failed\n"
          << x;
      }
      catch (...) {
        throw;
      }
    }
    actReg_.sPostSourceConstruction.invoke(input_->moduleDescription());
    // Create product tables used for product retrieval within modules.
    producedProductLookupTables_ = ProductTables{producedProductDescriptions_};
    outputCallbacks_->invoke(producedProductLookupTables_);
  }

  void
  EventProcessor::invokePostBeginJobWorkers_()
  {
    using cet::transform_all;
    // Need to convert multiple lists of workers into a long list that
    // the postBeginJobWorkers callbacks can understand.
    vector<Worker*> allWorkers;
    transform_all(pathManager_->triggerPathsInfo(ScheduleID::first()).workers(),
                  back_inserter(allWorkers),
                  [](auto const& pr) { return pr.second.get(); });
    transform_all(pathManager_->endPathInfo(ScheduleID::first()).workers(),
                  back_inserter(allWorkers),
                  [](auto const& pr) { return pr.second.get(); });
    actReg_.sPostBeginJobWorkers.invoke(input_, allWorkers);
  }

  //================================================================
  // Event-loop infrastructure

  template <Level L>
  bool
  EventProcessor::levelsToProcess()
  {
    if (nextLevel_.load() == Level::ReadyToAdvance) {
      nextLevel_ = advanceItemType();
      // Consider reading right here?
    }
    if (nextLevel_.load() == L) {
      nextLevel_ = Level::ReadyToAdvance;
      if (main_schedule().outputsToClose()) {
        setOutputFileStatus(OutputFileStatus::Switching);
        finalizeContainingLevels<L>();
        closeSomeOutputFiles();
      }
      return true;
    }
    if (nextLevel_.load() < L) {
      return false;
    }
    if (nextLevel_.load() == highest_level()) {
      return false;
    }
    throw Exception{errors::LogicError} << "Incorrect level hierarchy.\n"
                                        << "  Current level: " << L
                                        << "  Next level: " << nextLevel_;
  }

  // Specializations for process function template

  template <>
  inline void
  EventProcessor::begin<Level::Job>()
  {
    timer_->start();
    beginJob();
  }

  template <>
  inline void
  EventProcessor::begin<Level::InputFile>()
  {
    openInputFile();
  }

  template <>
  void
  EventProcessor::begin<Level::Run>()
  {
    readRun();

    // We only enable run finalization if reading was successful.
    // This appears to be a design weakness.
    finalizeRunEnabled_ = true;
    if (handleEmptyRuns_) {
      beginRun();
    }
  }

  template <>
  void
  EventProcessor::begin<Level::SubRun>()
  {
    assert(runPrincipal_);
    assert(runPrincipal_->runID().isValid());
    readSubRun();

    // We only enable subrun finalization if reading was successful.
    // This appears to be a design weakness.
    finalizeSubRunEnabled_ = true;
    if (handleEmptySubRuns_) {
      beginRunIfNotDoneAlready();
      beginSubRun();
    }
  }

  template <>
  void
  EventProcessor::finalize<Level::SubRun>()
  {
    if (!finalizeSubRunEnabled_) {
      return;
    }

    assert(subRunPrincipal_);
    if (subRunPrincipal_->subRunID().isFlush()) {
      return;
    }

    openSomeOutputFiles();
    setSubRunAuxiliaryRangeSetID();
    if (beginSubRunCalled_) {
      endSubRun();
    }
    writeSubRun();
    finalizeSubRunEnabled_ = false;
  }

  template <>
  void
  EventProcessor::finalize<Level::Run>()
  {
    if (!finalizeRunEnabled_) {
      return;
    }

    assert(runPrincipal_);
    if (runPrincipal_->runID().isFlush()) {
      return;
    }

    openSomeOutputFiles();
    setRunAuxiliaryRangeSetID();
    if (beginRunCalled_) {
      endRun();
    }
    writeRun();
    finalizeRunEnabled_ = false;
  }

  template <>
  void
  EventProcessor::finalize<Level::InputFile>()
  {
    if (nextLevel_.load() == Level::Job) {
      closeAllFiles();
    } else {
      closeInputFile();
    }
  }

  template <>
  void
  EventProcessor::finalize<Level::Job>()
  {
    endJob();
    timer_->stop();
  }

  template <>
  void
  EventProcessor::finalizeContainingLevels<Level::SubRun>()
  {
    finalize<Level::Run>();
  }

  template <>
  void
  EventProcessor::finalizeContainingLevels<Level::Event>()
  {
    finalize<Level::SubRun>();
    finalize<Level::Run>();
  }

  template <>
  void
  EventProcessor::recordOutputModuleClosureRequests<Level::Run>()
  {
    main_schedule().recordOutputClosureRequests(Granularity::Run);
  }

  template <>
  void
  EventProcessor::recordOutputModuleClosureRequests<Level::SubRun>()
  {
    main_schedule().recordOutputClosureRequests(Granularity::SubRun);
  }

  template <>
  void
  EventProcessor::recordOutputModuleClosureRequests<Level::Event>()
  {
    main_schedule().recordOutputClosureRequests(Granularity::Event);
  }

  //=============================================
  // Job level

  void
  EventProcessor::beginJob()
  {
    FDEBUG(1) << string(8, ' ') << "beginJob\n";
    breakpoints::beginJob();
    // NOTE: This implementation assumes 'Job' means one call the
    // EventProcessor::run. If it really means once per 'application'
    // then this code will have to be changed.  Also have to deal with
    // case where have 'run' then new Module added and do 'run' again.
    // In that case the newly added Module needs its 'beginJob' to be
    // called.
    try {
      input_->doBeginJob();
    }
    catch (cet::exception& e) {
      mf::LogError("BeginJob") << "A cet::exception happened while processing"
                                  " the beginJob of the 'source'";
      e << "A cet::exception happened while processing"
           " the beginJob of the 'source'\n";
      throw;
    }
    catch (exception const&) {
      mf::LogError("BeginJob") << "A exception happened while processing"
                                  " the beginJob of the 'source'";
      throw;
    }
    catch (...) {
      mf::LogError("BeginJob") << "An unknown exception happened while"
                                  " processing the beginJob of the 'source'";
      throw;
    }
    scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
      schedule(sid).beginJob(sharedResources_);
    });
    actReg_.sPostBeginJob.invoke();
    invokePostBeginJobWorkers_();
  }

  void
  EventProcessor::endJob()
  {
    FDEBUG(1) << string(8, ' ') << "endJob\n";
    ec_->call([this] { endJobAllSchedules(); });
    ec_->call([] { ConsumesInfo::instance()->showMissingConsumes(); });
    ec_->call([this] { input_->doEndJob(); });
    ec_->call([this] { actReg_.sPostEndJob.invoke(); });
    ec_->call([] { mf::LogStatistics(); });
    ec_->call([this] {
      detail::writeSummary(pathManager_, scheduler_->wantSummary(), timer_);
    });
  }

  void
  EventProcessor::endJobAllSchedules()
  {
    scheduleIteration_.for_each_schedule(
      [this](ScheduleID const sid) { schedule(sid).endJob(); });
  }

  //====================================================
  // File level

  void
  EventProcessor::openInputFile()
  {
    actReg_.sPreOpenFile.invoke();
    FDEBUG(1) << string(8, ' ') << "openInputFile\n";
    fb_.reset(input_->readFile().release());
    if (fb_ == nullptr) {
      throw Exception(errors::LogicError)
        << "Source readFile() did not return a valid FileBlock: FileBlock "
        << "should be valid or readFile() should throw.\n";
    }
    actReg_.sPostOpenFile.invoke(fb_->fileName());
    respondToOpenInputFile();
  }

  void
  EventProcessor::closeAllFiles()
  {
    closeAllOutputFiles();
    closeInputFile();
  }

  void
  EventProcessor::closeInputFile()
  {
    main_schedule().incrementInputFileNumber();
    // Output-file closing on input-file boundaries are tricky since
    // input files must outlive the output files, which often have
    // data copied forward from the input files.  That's why the
    // recordOutputClosureRequests call is made here instead of in a
    // specialization of recordOutputModuleClosureRequests<>.
    main_schedule().recordOutputClosureRequests(Granularity::InputFile);
    if (main_schedule().outputsToClose()) {
      closeSomeOutputFiles();
    }
    respondToCloseInputFile();
    actReg_.sPreCloseFile.invoke();
    input_->closeFile();
    actReg_.sPostCloseFile.invoke();
    FDEBUG(1) << string(8, ' ') << "closeInputFile\n";
  }

  void
  EventProcessor::closeAllOutputFiles()
  {
    if (!main_schedule().someOutputsOpen()) {
      return;
    }
    respondToCloseOutputFiles();
    main_schedule().closeAllOutputFiles();
    FDEBUG(1) << string(8, ' ') << "closeAllOutputFiles\n";
  }

  bool
  EventProcessor::outputsToOpen()
  {
    bool outputs_to_open{false};
    auto check_outputs_to_open = [this,
                                  &outputs_to_open](ScheduleID const sid) {
      if (schedule(sid).outputsToOpen()) {
        outputs_to_open = true;
      }
    };
    scheduleIteration_.for_each_schedule(check_outputs_to_open);
    return outputs_to_open;
  }

  void
  EventProcessor::openSomeOutputFiles()
  {
    if (!outputsToOpen()) {
      return;
    }

    auto open_some_outputs = [this](ScheduleID const sid) {
      schedule(sid).openSomeOutputFiles(*fb_);
    };
    scheduleIteration_.for_each_schedule(open_some_outputs);

    FDEBUG(1) << string(8, ' ') << "openSomeOutputFiles\n";
    respondToOpenOutputFiles();
  }

  void
  EventProcessor::setOutputFileStatus(OutputFileStatus const ofs)
  {
    main_schedule().setOutputFileStatus(ofs);
    FDEBUG(1) << string(8, ' ') << "setOutputFileStatus\n";
  }

  void
  EventProcessor::closeSomeOutputFiles()
  {
    // Precondition: there are SOME output files that have been
    //               flagged as needing to close.  Otherwise,
    //               'respondtoCloseOutputFiles' will be needlessly
    //               called.
    assert(main_schedule().outputsToClose());
    respondToCloseOutputFiles();
    main_schedule().closeSomeOutputFiles();
    FDEBUG(1) << string(8, ' ') << "closeSomeOutputFiles\n";
  }

  void
  EventProcessor::respondToOpenInputFile()
  {
    scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
      schedule(sid).respondToOpenInputFile(*fb_);
    });
    FDEBUG(1) << string(8, ' ') << "respondToOpenInputFile\n";
  }

  void
  EventProcessor::respondToCloseInputFile()
  {
    scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
      schedule(sid).respondToCloseInputFile(*fb_);
    });
    FDEBUG(1) << string(8, ' ') << "respondToCloseInputFile\n";
  }

  void
  EventProcessor::respondToOpenOutputFiles()
  {
    scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
      schedule(sid).respondToOpenOutputFiles(*fb_);
    });
    FDEBUG(1) << string(8, ' ') << "respondToOpenOutputFiles\n";
  }

  void
  EventProcessor::respondToCloseOutputFiles()
  {
    scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
      schedule(sid).respondToCloseOutputFiles(*fb_);
    });
    FDEBUG(1) << string(8, ' ') << "respondToCloseOutputFiles\n";
  }

  //=============================================
  // Run level

  void
  EventProcessor::readRun()
  {
    actReg_.sPreSourceRun.invoke();
    runPrincipal_.reset(input_->readRun().release());
    assert(runPrincipal_);
    auto rsh = input_->runRangeSetHandler();
    assert(rsh);
    auto seed_range_set = [this, &rsh](ScheduleID const sid) {
      schedule(sid).seedRunRangeSet(*rsh);
    };
    scheduleIteration_.for_each_schedule(seed_range_set);
    // The intended behavior here is that the producing services which
    // are called during the sPostReadRun cannot see each others put
    // products. We enforce this by creating the groups for the
    // produced products, but do not allow the lookups to find them
    // until after the callbacks have run.
    runPrincipal_->createGroupsForProducedProducts(
      producedProductLookupTables_);
    psSignals_->sPostReadRun.invoke(*runPrincipal_);
    runPrincipal_->enableLookupOfProducedProducts();
    {
      auto const r =
        std::as_const(*runPrincipal_).makeRun(invalid_module_context);
      actReg_.sPostSourceRun.invoke(r);
    }
    FDEBUG(1) << string(8, ' ') << "readRun.....................("
              << runPrincipal_->runID() << ")\n";
  }

  void
  EventProcessor::beginRun()
  {
    assert(runPrincipal_);
    RunID const r{runPrincipal_->runID()};
    if (r.isFlush()) {
      return;
    }
    finalizeRunEnabled_ = true;
    try {
      {
        auto const run =
          std::as_const(*runPrincipal_).makeRun(invalid_module_context);
        actReg_.sPreBeginRun.invoke(run);
      }
      scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
        schedule(sid).process(Transition::BeginRun, *runPrincipal_);
      });
      {
        auto const run =
          std::as_const(*runPrincipal_).makeRun(invalid_module_context);
        actReg_.sPostBeginRun.invoke(run);
      }
    }
    catch (cet::exception& ex) {
      throw Exception{
        errors::EventProcessorFailure,
        "EventProcessor: an exception occurred during current event processing",
        ex};
    }
    catch (...) {
      mf::LogError("PassingThrough")
        << "an exception occurred during current event processing";
      throw;
    }
    FDEBUG(1) << string(8, ' ') << "beginRun....................(" << r
              << ")\n";
    beginRunCalled_ = true;
  }

  void
  EventProcessor::beginRunIfNotDoneAlready()
  {
    if (!beginRunCalled_) {
      beginRun();
    }
  }

  void
  EventProcessor::setRunAuxiliaryRangeSetID()
  {
    assert(runPrincipal_);
    FDEBUG(1) << string(8, ' ') << "setRunAuxiliaryRangeSetID...("
              << runPrincipal_->runID() << ")\n";
    if (main_schedule().runRangeSetHandler().type() ==
        RangeSetHandler::HandlerType::Open) {
      // We are using EmptyEvent source, need to merge what the
      // schedules have seen.
      auto mergedRS = RangeSet::invalid();
      auto merge_range_sets = [this, &mergedRS](ScheduleID const sid) {
        auto const& rs = schedule(sid).runRangeSetHandler().seenRanges();
        // The following constructor ensures that the range is sorted
        // before 'merge' is called.
        RangeSet const tmp{rs.run(), rs.ranges()};
        mergedRS.merge(tmp);
      };
      scheduleIteration_.for_each_schedule(merge_range_sets);
      runPrincipal_->updateSeenRanges(mergedRS);
      auto update_executors = [this, &mergedRS](ScheduleID const sid) {
        schedule(sid).setRunAuxiliaryRangeSetID(mergedRS);
      };
      scheduleIteration_.for_each_schedule(update_executors);
      return;
    }

    // Since we are using already existing ranges, all the range set
    // handlers have the same ranges, use the first one.  handler with
    // the largest event number, that will be the one which we will
    // use as the file switch boundary.  Note that is may not match
    // the exactly the schedule that triggered the switch.  Do we need
    // to fix this?
    unique_ptr<RangeSetHandler> rshAtSwitch{
      main_schedule().runRangeSetHandler().clone()};
    if (main_schedule().fileStatus() != OutputFileStatus::Switching) {
      // We are at the end of the job.
      rshAtSwitch->flushRanges();
    }
    runPrincipal_->updateSeenRanges(rshAtSwitch->seenRanges());
    main_schedule().setRunAuxiliaryRangeSetID(rshAtSwitch->seenRanges());
  }

  void
  EventProcessor::endRun()
  {
    assert(runPrincipal_);
    // Precondition: The RunID does not correspond to a flush ID. --
    // N.B. The flush flag is not explicitly checked here since endRun
    // is only called from finalizeRun, which is where the check
    // happens.
    RunID const run{runPrincipal_->runID()};
    assert(!run.isFlush());
    try {
      actReg_.sPreEndRun.invoke(runPrincipal_->runID(),
                                runPrincipal_->endTime());
      scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
        schedule(sid).process(Transition::EndRun, *runPrincipal_);
      });
      auto const r =
        std::as_const(*runPrincipal_).makeRun(invalid_module_context);
      actReg_.sPostEndRun.invoke(r);
    }
    catch (cet::exception& ex) {
      throw Exception{
        errors::EventProcessorFailure,
        "EventProcessor: an exception occurred during current event processing",
        ex};
    }
    catch (...) {
      mf::LogError("PassingThrough")
        << "an exception occurred during current event processing";
      throw;
    }
    FDEBUG(1) << string(8, ' ') << "endRun......................(" << run
              << ")\n";
    beginRunCalled_ = false;
  }

  void
  EventProcessor::writeRun()
  {
    assert(runPrincipal_);
    // Precondition: The RunID does not correspond to a flush ID.
    RunID const r{runPrincipal_->runID()};
    assert(!r.isFlush());
    main_schedule().writeRun(*runPrincipal_);
    FDEBUG(1) << string(8, ' ') << "writeRun....................(" << r
              << ")\n";
  }

  //=============================================
  // SubRun level

  void
  EventProcessor::readSubRun()
  {
    actReg_.sPreSourceSubRun.invoke();
    subRunPrincipal_.reset(input_->readSubRun(runPrincipal_.get()).release());
    assert(subRunPrincipal_);
    auto rsh = input_->subRunRangeSetHandler();
    assert(rsh);
    auto seed_range_set = [this, &rsh](ScheduleID const sid) {
      schedule(sid).seedSubRunRangeSet(*rsh);
    };
    scheduleIteration_.for_each_schedule(seed_range_set);
    // The intended behavior here is that the producing services which
    // are called during the sPostReadSubRun cannot see each others
    // put products. We enforce this by creating the groups for the
    // produced products, but do not allow the lookups to find them
    // until after the callbacks have run.
    subRunPrincipal_->createGroupsForProducedProducts(
      producedProductLookupTables_);
    psSignals_->sPostReadSubRun.invoke(*subRunPrincipal_);
    subRunPrincipal_->enableLookupOfProducedProducts();
    {
      auto const sr =
        std::as_const(*subRunPrincipal_).makeSubRun(invalid_module_context);
      actReg_.sPostSourceSubRun.invoke(sr);
    }
    FDEBUG(1) << string(8, ' ') << "readSubRun..................("
              << subRunPrincipal_->subRunID() << ")\n";
  }

  void
  EventProcessor::beginSubRun()
  {
    assert(subRunPrincipal_);
    SubRunID const sr{subRunPrincipal_->subRunID()};
    if (sr.isFlush()) {
      return;
    }
    finalizeSubRunEnabled_ = true;
    try {
      {
        auto const srun =
          std::as_const(*subRunPrincipal_).makeSubRun(invalid_module_context);
        actReg_.sPreBeginSubRun.invoke(srun);
      }
      scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
        schedule(sid).process(Transition::BeginSubRun, *subRunPrincipal_);
      });
      {
        auto const srun =
          std::as_const(*subRunPrincipal_).makeSubRun(invalid_module_context);
        actReg_.sPostBeginSubRun.invoke(srun);
      }
    }
    catch (cet::exception& ex) {
      throw Exception{
        errors::EventProcessorFailure,
        "EventProcessor: an exception occurred during current event processing",
        ex};
    }
    catch (...) {
      mf::LogError("PassingThrough")
        << "an exception occurred during current event processing";
      throw;
    }
    FDEBUG(1) << string(8, ' ') << "beginSubRun.................(" << sr
              << ")\n";
    beginSubRunCalled_ = true;
  }

  void
  EventProcessor::beginSubRunIfNotDoneAlready()
  {
    if (!beginSubRunCalled_) {
      beginSubRun();
    }
  }

  void
  EventProcessor::setSubRunAuxiliaryRangeSetID()
  {
    assert(subRunPrincipal_);
    FDEBUG(1) << string(8, ' ') << "setSubRunAuxiliaryRangeSetID("
              << subRunPrincipal_->subRunID() << ")\n";
    if (main_schedule().subRunRangeSetHandler().type() ==
        RangeSetHandler::HandlerType::Open) {
      // We are using EmptyEvent source, need to merge what the
      // schedules have seen.
      auto mergedRS = RangeSet::invalid();
      auto merge_range_sets = [this, &mergedRS](ScheduleID const sid) {
        auto const& rs = schedule(sid).subRunRangeSetHandler().seenRanges();
        // The following constructor ensures that the range is sorted
        // before 'merge' is called.
        RangeSet const tmp{rs.run(), rs.ranges()};
        mergedRS.merge(tmp);
      };
      scheduleIteration_.for_each_schedule(merge_range_sets);
      subRunPrincipal_->updateSeenRanges(mergedRS);
      auto update_executors = [this, &mergedRS](ScheduleID const sid) {
        schedule(sid).setSubRunAuxiliaryRangeSetID(mergedRS);
      };
      scheduleIteration_.for_each_schedule(update_executors);
      return;
    }
    // Ranges are split/flushed only for a RangeSetHandler whose
    // dynamic type is 'ClosedRangeSetHandler'.
    //
    // Consider the following range-sets
    //
    //  SubRun RangeSet:
    //
    //    { Run 1 : SubRun 1 : Events [1,7) }  <-- Current
    //
    //  Run RangeSet:
    //
    //    { Run 1 : SubRun 0 : Events [5,11)
    //              SubRun 1 : Events [1,7)    <-- Current
    //              SubRun 1 : Events [9,15) }
    //
    // For a range split just before SubRun 1, Event 6, the
    // range sets should become:
    //
    //  SubRun RangeSet:
    //
    //    { Run 1 : SubRun 1 : Events [1,6)
    //              SubRun 1 : Events [6,7) } <-- Updated
    //
    //  Run RangeSet:
    //
    //    { Run 1 : SubRun 0 : Events [5,11)
    //              SubRun 1 : Events [1,6)
    //              SubRun 1 : Events [6,7)   <-- Updated
    //              SubRun 1 : Events [9,15) }
    //
    // Since we are using already existing ranges, all the range set
    // handlers have the same ranges.  Find the closed range set
    // handler with the largest event number, that will be the one
    // which we will use as the file switch boundary.  Note that is
    // may not match the exactly the schedule that triggered the
    // switch.  Do we need to fix this?
    //
    // If we do not find any handlers with valid event info then we
    // use the first one, which is just fine.  This happens for
    // example when we are dropping all events.
    unsigned largestEvent = 1U;
    ScheduleID idxOfMax{ScheduleID::first()};
    ScheduleID idx{ScheduleID::first()};
    auto& val = main_schedule().subRunRangeSetHandler();
    auto& rsh = dynamic_cast<ClosedRangeSetHandler const&>(val);
    // Make sure the event number is a valid event number before using
    // it. It can be invalid in the handler if we have not yet read an
    // event, which happens with empty subruns and when we are
    // dropping all events.
    if (rsh.eventInfo().id().isValid() && !rsh.eventInfo().id().isFlush()) {
      if (rsh.eventInfo().id().event() > largestEvent) {
        largestEvent = rsh.eventInfo().id().event();
        idxOfMax = idx;
      }
    }
    idx = idx.next();

    unique_ptr<RangeSetHandler> rshAtSwitch{
      main_schedule().subRunRangeSetHandler().clone()};
    if (main_schedule().fileStatus() == OutputFileStatus::Switching) {
      rshAtSwitch->maybeSplitRange();
      unique_ptr<RangeSetHandler> runRSHAtSwitch{
        schedule(idxOfMax).runRangeSetHandler().clone()};
      runRSHAtSwitch->maybeSplitRange();
      main_schedule().seedRunRangeSet(*runRSHAtSwitch);
    } else {
      // We are at the end of the job.
      rshAtSwitch->flushRanges();
    }
    main_schedule().seedSubRunRangeSet(*rshAtSwitch);
    subRunPrincipal_->updateSeenRanges(rshAtSwitch->seenRanges());
    main_schedule().setSubRunAuxiliaryRangeSetID(rshAtSwitch->seenRanges());
  }

  void
  EventProcessor::endSubRun()
  {
    assert(subRunPrincipal_);
    // Precondition: The SubRunID does not correspond to a flush ID.
    // Note: the flush flag is not explicitly checked here since
    // endSubRun is only called from finalizeSubRun, which is where the
    // check happens.
    SubRunID const sr{subRunPrincipal_->subRunID()};
    assert(!sr.isFlush());
    try {
      actReg_.sPreEndSubRun.invoke(subRunPrincipal_->subRunID(),
                                   subRunPrincipal_->endTime());
      scheduleIteration_.for_each_schedule([this](ScheduleID const sid) {
        schedule(sid).process(Transition::EndSubRun, *subRunPrincipal_);
      });
      auto const srun =
        std::as_const(*subRunPrincipal_).makeSubRun(invalid_module_context);
      actReg_.sPostEndSubRun.invoke(srun);
    }
    catch (cet::exception& ex) {
      throw Exception{
        errors::EventProcessorFailure,
        "EventProcessor: an exception occurred during current event processing",
        ex};
    }
    catch (...) {
      mf::LogError("PassingThrough")
        << "an exception occurred during current event processing";
      throw;
    }
    FDEBUG(1) << string(8, ' ') << "endSubRun...................(" << sr
              << ")\n";
    beginSubRunCalled_ = false;
  }

  void
  EventProcessor::writeSubRun()
  {
    assert(subRunPrincipal_);
    // Precondition: The SubRunID does not correspond to a flush ID.
    SubRunID const& sr{subRunPrincipal_->subRunID()};
    assert(!sr.isFlush());
    main_schedule().writeSubRun(*subRunPrincipal_);
    FDEBUG(1) << string(8, ' ') << "writeSubRun.................(" << sr
              << ")\n";
  }

  // ==============================================================================
  // Event level

  template <>
  void
  EventProcessor::process<most_deeply_nested_level()>()
  {
    if ((shutdown_flag > 0) || !ec_->empty()) {
      return;
    }
    // Note: This loop is to allow output file switching to happen in
    // the main thread.
    firstEvent_ = true;
    bool done = false;
    while (!done) {
      beginRunIfNotDoneAlready();
      beginSubRunIfNotDoneAlready();

      auto const last_schedule_index = scheduler_->num_schedules() - 1;
      for (ScheduleID::size_type i = 0; i != last_schedule_index; ++i) {
        taskGroup_->run([this, i] { processAllEventsAsync(ScheduleID(i)); });
      }
      taskGroup_->native_group().run_and_wait([this, last_schedule_index] {
        processAllEventsAsync(ScheduleID(last_schedule_index));
      });

      // If anything bad happened during event processing, let the
      // user know.
      sharedException_.throw_if_stored_exception();
      if (!fileSwitchInProgress_.load()) {
        done = true;
        continue;
      }
      setOutputFileStatus(OutputFileStatus::Switching);
      finalizeContainingLevels<most_deeply_nested_level()>();
      respondToCloseOutputFiles();
      main_schedule().closeSomeOutputFiles();
      FDEBUG(1) << string(8, ' ') << "closeSomeOutputFiles\n";
      // We started the switch after advancing to the next item type;
      // we must make sure that we read that event before advancing
      // the item type again.
      firstEvent_ = true;
      fileSwitchInProgress_ = false;
    }
  }

  // This is the event loop (also known as the schedule head).  It
  // calls readAndProcessAsync, which reads and processes a single
  // event, creates itself again as a continuation task, and then
  // exits.
  void
  EventProcessor::processAllEventsAsync(ScheduleID const sid)
  {
    // Note: We are part of the processAllEventsTask (schedule head
    // task), and our parent is the eventLoopTask.
    TDEBUG_BEGIN_FUNC_SI(4, sid);
    try {
      readAndProcessAsync(sid);
    }
    catch (...) {
      sharedException_.store_current();
      TDEBUG_END_FUNC_SI(4, sid) << "terminate event loop because of EXCEPTION";
      return;
    }
    // If no exception, then end this task, which does not terminate
    // event processing because readAndProcessAsync creates a
    // continuation task.
    TDEBUG_END_FUNC_SI(4, sid);
  }

  // This function is executed as part of the readAndProcessEvent
  // task, our parent task is the EventLoopTask. Here we advance to
  // the next item in the file index, end event processing if it is
  // not an event, or if the user has requested a shutdown, read the
  // event, and then call another function to do the processing.
  void
  EventProcessor::readAndProcessAsync(ScheduleID const sid)
  {
    // Note: We are part of the readAndProcessEventTask (schedule head
    // task), and our parent task is the eventLoopTask.
    TDEBUG_BEGIN_FUNC_SI(4, sid);
    // Note: shutdown_flag is a extern global atomic int in
    // art/art/Utilities/UnixSignalHandlers.cc
    if (shutdown_flag) {
      // User called for a clean shutdown using a signal or ctrl-c,
      // end event processing and this task.
      TDEBUG_END_FUNC_SI(4, sid) << "CLEAN SHUTDOWN";
      return;
    }

    // The item type advance and the event read must be done with the
    // input source lock held; however event-processing must not
    // serialized.
    {
      InputSourceMutexSentry lock_input;
      if (fileSwitchInProgress_.load()) {
        // We must avoid advancing the iterator after a schedule has
        // noticed it is time to switch files.  After the switch, we
        // will need to set firstEvent_ true so that the first
        // schedule that resumes after the switch actually reads the
        // event that the first schedule which noticed we needed a
        // switch had advanced the iterator to.

        // Note: We still have the problem that because the schedules
        // do not read events at the same time the file switch point
        // can be up to nschedules-1 ahead of where it would have been
        // if there was only one schedule.  If we are switching output
        // files every event in an attempt to create single event
        // files, this really does not work out too well.
        TDEBUG_END_FUNC_SI(4, sid) << "FILE SWITCH";
        return;
      }
      // Check the next item type and exit this task if it is not an
      // event, or if the user has asynchronously requested a
      // shutdown.
      auto expected = true;
      if (firstEvent_.compare_exchange_strong(expected, false)) {
        // Do not advance the item type on the first event.
      } else {
        // Do the advance item type.
        if (nextLevel_.load() == Level::ReadyToAdvance) {
          // See what the next item is.
          TDEBUG_FUNC_SI(5, sid) << "Calling advanceItemType()";
          nextLevel_ = advanceItemType();
        }
        if ((nextLevel_.load() < most_deeply_nested_level()) ||
            (nextLevel_.load() == highest_level())) {
          // We are popping up, end event processing and this task.
          TDEBUG_END_FUNC_SI(4, sid) << "END OF SUBRUN";
          return;
        }
        if (nextLevel_.load() != most_deeply_nested_level()) {
          // Error: incorrect level hierarchy
          TDEBUG_END_FUNC_SI(4, sid) << "BAD HIERARCHY";
          throw Exception{errors::LogicError} << "Incorrect level hierarchy.";
        }
        nextLevel_ = Level::ReadyToAdvance;
        // At this point we have determined that we are going to read
        // an event and we must do that before dropping the lock on
        // the input source which is what is protecting us against a
        // double-advance caused by a different schedule.
        if (schedule(sid).outputsToClose()) {
          fileSwitchInProgress_ = true;
          TDEBUG_END_FUNC_SI(4, sid) << "FILE SWITCH INITIATED";
          return;
        }
      }

      // Now we can read the event from the source.
      ScheduleContext const sc{sid};
      assert(subRunPrincipal_);
      assert(subRunPrincipal_->subRunID().isValid());
      actReg_.sPreSourceEvent.invoke(sc);
      TDEBUG_FUNC_SI(5, sid) << "Calling input_->readEvent(subRunPrincipal_)";
      auto ep = input_->readEvent(subRunPrincipal_.get());
      assert(ep);
      // The intended behavior here is that the producing services
      // which are called during the sPostReadEvent cannot see each
      // others put products.  We enforce this by creating the groups
      // for the produced products, but do not allow the lookups to
      // find them until after the callbacks have run.
      ep->createGroupsForProducedProducts(producedProductLookupTables_);
      psSignals_->sPostReadEvent.invoke(*ep);
      ep->enableLookupOfProducedProducts();
      actReg_.sPostSourceEvent.invoke(
        std::as_const(*ep).makeEvent(invalid_module_context), sc);
      FDEBUG(1) << string(8, ' ') << "readEvent...................("
                << ep->eventID() << ")\n";
      schedule(sid).accept_principal(move(ep));
      // Now we drop the input source lock by exiting the guarded
      // scope.
    }
    if (schedule(sid).event_principal().eventID().isFlush()) {
      // No processing to do, start next event handling task.
      processAllEventsAsync(sid);
      TDEBUG_END_FUNC_SI(4, sid) << "FLUSH EVENT";
      return;
    }

    // Now process the event.
    processEventAsync(sid);
    TDEBUG_END_FUNC_SI(4, sid);
  }

  // ----------------------------------------------------------------------------
  class EventProcessor::EndPathRunnerTask {
  public:
    EndPathRunnerTask(EventProcessor* evp, ScheduleID const sid)
      : evp_{evp}, sid_{sid}
    {}

    void
    operator()(std::exception_ptr ex) const
    {
      TDEBUG_BEGIN_TASK_SI(4, sid_);
      if (ex) {
        try {
          rethrow_exception(ex);
        }
        catch (cet::exception& e) {
          if (evp_->error_action(e) != actions::IgnoreCompletely) {
            evp_->sharedException_.store<Exception>(
              errors::EventProcessorFailure,
              "EventProcessor: an exception occurred during current "
              "event processing",
              e);
            TDEBUG_END_TASK_SI(4, sid_);
            return;
          }
          mf::LogWarning(e.category())
            << "exception being ignored for current event:\n"
            << cet::trim_right_copy(e.what(), " \n");
          // WARNING: We continue processing after the catch blocks!!!
        }
        catch (...) {
          mf::LogError("PassingThrough")
            << "an exception occurred during current event processing";
          evp_->sharedException_.store_current();
          TDEBUG_END_TASK_SI(4, sid_);
          return;
        }
      }

      evp_->finishEventAsync(sid_);

      TDEBUG_END_TASK_SI(4, sid_);
    }

  private:
    EventProcessor* evp_;
    ScheduleID const sid_;
  };

  // ----------------------------------------------------------------------------
  class EventProcessor::EndPathTask {
  public:
    EndPathTask(EventProcessor* evp, ScheduleID const sid)
      : evp_{evp}, sid_{sid}
    {}

    void
    operator()(exception_ptr const ex)
    {
      // Note: When we start our parent is the eventLoopTask.
      TDEBUG_BEGIN_TASK_SI(4, sid_);
      if (ex) {
        try {
          rethrow_exception(ex);
        }
        catch (cet::exception& e) {
          auto const action = evp_->error_action(e);
          if (action != actions::IgnoreCompletely) {
            assert(action != actions::FailModule);
            assert(action != actions::FailPath);
            if (action == actions::SkipEvent) {
              mf::LogWarning(e.category())
                << "Skipping event due to the following exception:\n"
                << cet::trim_right_copy(e.what(), " \n");
              TDEBUG_END_TASK_SI(4, sid_)
                << "skipping event because of EXCEPTION";
              return;
            }
            evp_->sharedException_.store<Exception>(
              errors::EventProcessorFailure,
              "EventProcessor: an exception occurred during current "
              "event processing",
              e);
            TDEBUG_END_TASK_SI(4, sid_)
              << "terminate event loop because of EXCEPTION";
            return;
          }
          mf::LogWarning(e.category())
            << "exception being ignored for current event:\n"
            << cet::trim_right_copy(e.what(), " \n");
          // WARNING: We continue processing after the catch blocks!!!
        }
        catch (...) {
          mf::LogError("PassingThrough")
            << "an exception occurred during current event processing";
          evp_->sharedException_.store_current();
          TDEBUG_END_TASK_SI(4, sid_)
            << "terminate event loop because of EXCEPTION";
          return;
        }
      }

      auto finalize_event_task =
        make_waiting_task<EndPathRunnerTask>(evp_, sid_);
      try {
        evp_->schedule(sid_).process_event_observers(finalize_event_task);
      }
      catch (cet::exception& e) {
        if (evp_->error_action(e) != actions::IgnoreCompletely) {
          evp_->sharedException_.store<Exception>(
            errors::EventProcessorFailure,
            "EventProcessor: an exception occurred during current event "
            "processing",
            e);
          TDEBUG_END_TASK_SI(4, sid_)
            << "terminate event loop because of EXCEPTION";
          return;
        }
        mf::LogWarning(e.category())
          << "exception being ignored for current event:\n"
          << cet::trim_right_copy(e.what(), " \n");
        // WARNING: We continue processing after the catch blocks!!!
      }
      catch (...) {
        mf::LogError("PassingThrough")
          << "an exception occurred during current event processing";
        evp_->sharedException_.store_current();
        TDEBUG_END_TASK_SI(4, sid_)
          << "terminate event loop because of EXCEPTION";
        return;
      }

      // Once the end path processing is done, exit this task, which
      // does not end event-processing because of the continuation
      // task.
      TDEBUG_END_TASK_SI(4, sid_);
    }

  private:
    EventProcessor* evp_;
    ScheduleID const sid_;
  };

  // This function is a continuation of the body of the
  // readAndProcessEvent task. Here we call down to Schedule to do the
  // trigger path processing, passing it a waiting task which will do
  // the end path processing, finalize the event, and start the next
  // read and process event task.  Note that Schedule will spawn a
  // task to process each of the trigger paths, and then when they are
  // finished, insert the trigger results, and then spawn the waiting
  // task we gave it to do the end path processing, write the event,
  // and then start the next event processing task.
  void
  EventProcessor::processEventAsync(ScheduleID const sid) try {
    // Note: We are part of the readAndProcessEventTask (schedule head
    // task), and our parent task is the eventLoopTask.
    TDEBUG_BEGIN_FUNC_SI(4, sid);
    assert(!schedule(sid).event_principal().eventID().isFlush());
    // Continue processing via the creation of a continuation.
    auto endPathTask = make_waiting_task<EndPathTask>(this, sid);
    // Start the trigger paths running.  When they finish they will
    // spawn the endPathTask which will run the end path, write the
    // event, and start the next event processing task.
    schedule(sid).process_event_modifiers(endPathTask);
    TDEBUG_END_FUNC_SI(4, sid);
  }
  catch (cet::exception& e) {
    // Upon exiting this scope, end this task, terminating event
    // processing.
    auto const action = error_action(e);
    if (action != actions::IgnoreCompletely) {
      assert(action != actions::FailModule);
      assert(action != actions::FailPath);
      sharedException_.store<Exception>(
        errors::EventProcessorFailure,
        "EventProcessor: an exception occurred during current event processing",
        e);
      TDEBUG_END_FUNC_SI(4, sid) << "terminate event loop because of EXCEPTION";
      return;
    }
    mf::LogWarning(e.category())
      << "exception being ignored for current event:\n"
      << cet::trim_right_copy(e.what(), " \n");
    TDEBUG_END_FUNC_SI(4, sid) << "Ignoring exception.";
  }
  catch (...) {
    mf::LogError("PassingThrough")
      << "an exception occurred during current event processing";
    sharedException_.store_current();
    TDEBUG_END_FUNC_SI(4, sid) << "terminate event loop because of EXCEPTION";
  }

  void
  EventProcessor::finishEventAsync(ScheduleID const sid)
  {
    auto& ep = schedule(sid).event_principal();
    actReg_.sPostProcessEvent.invoke(
      std::as_const(ep).makeEvent(invalid_module_context),
      ScheduleContext{sid});

    // Note: We are part of the endPathTask.
    TDEBUG_BEGIN_FUNC_SI(4, sid);
    FDEBUG(1) << string(8, ' ') << "processEvent................("
              << ep.eventID() << ")\n";
    try {
      // Ask the output workers if they have reached their limits, and
      // if so setup to end the job the next time around the event
      // loop.
      FDEBUG(1) << string(8, ' ') << "shouldWeStop\n";
      static std::mutex m;
      std::lock_guard sentry{m};
      // Now we can write the results of processing to the outputs,
      // and delete the event principal.
      if (!ep.eventID().isFlush()) {
        // Possibly open new output files.  This is safe to do because
        // EndPathExecutor functions are called in a serialized
        // context.
        TDEBUG_FUNC_SI(5, sid) << "Calling openSomeOutputFiles()";
        openSomeOutputFiles();
        TDEBUG_FUNC_SI(5, sid) << "Calling schedule(sid).writeEvent()";

        auto const id = ep.eventID();
        schedule(sid).writeEvent();
        FDEBUG(1) << string(8, ' ') << "writeEvent..................(" << id
                  << ")\n";
      }
      TDEBUG_FUNC_SI(5, sid)
        << "Calling schedules_->"
           "recordOutputClosureRequests(Granularity::Event)";
      schedule(sid).recordOutputClosureRequests(Granularity::Event);
    }
    catch (cet::exception& e) {
      if (error_action(e) != actions::IgnoreCompletely) {
        sharedException_.store<Exception>(
          errors::EventProcessorFailure,
          "EventProcessor: an exception occurred "
          "during current event processing",
          e);
        // And then end this task, terminating event processing.
        TDEBUG_END_FUNC_SI(4, sid) << "EXCEPTION";
        return;
      }
      mf::LogWarning(e.category())
        << "exception being ignored for current event:\n"
        << cet::trim_right_copy(e.what(), " \n");
      // WARNING: We continue processing after the catch blocks!!!
    }
    catch (...) {
      mf::LogError("PassingThrough")
        << "an exception occurred during current event processing";
      sharedException_.store_current();
      // And then end this task, terminating event processing.
      TDEBUG_END_FUNC_SI(4, sid) << "EXCEPTION";
      return;
    }

    // The next event processing task is a continuation of this task.
    processAllEventsAsync(sid);
    TDEBUG_END_FUNC_SI(4, sid);
  }

  template <Level L>
  void
  EventProcessor::process()
  {
    if ((shutdown_flag > 0) || !ec_->empty()) {
      return;
    }
    ec_->call([this] { begin<L>(); });
    while ((shutdown_flag == 0) && ec_->empty() &&
           levelsToProcess<level_down(L)>()) {
      ec_->call([this] { process<level_down(L)>(); });
    }
    ec_->call([this] {
      finalize<L>();
      recordOutputModuleClosureRequests<L>();
    });
  }

  EventProcessor::StatusCode
  EventProcessor::runToCompletion()
  {
    StatusCode returnCode{epSuccess};
    ec_->call([this, &returnCode] {
      process<highest_level()>();
      if (shutdown_flag > 0) {
        returnCode = epSignal;
      }
    });
    if (!ec_->empty()) {
      terminateAbnormally_();
      ec_->rethrow();
    }
    return returnCode;
  }

  Level
  EventProcessor::advanceItemType()
  {
    auto const itemType = input_->nextItemType();
    FDEBUG(1) << string(4, ' ') << "*** nextItemType: " << itemType << " ***\n";
    switch (itemType) {
    case input::IsStop:
      return highest_level();
    case input::IsFile:
      return Level::InputFile;
    case input::IsRun:
      return Level::Run;
    case input::IsSubRun:
      return Level::SubRun;
    case input::IsEvent:
      return Level::Event;
    case input::IsInvalid:
      throw Exception{errors::LogicError}
        << "Invalid next item type presented to the event processor.\n"
        << "Please contact artists@fnal.gov.";
    }
    throw Exception{errors::LogicError}
      << "Unrecognized next item type presented to the event processor.\n"
      << "Please contact artists@fnal.gov.";
  }

  // ===============================================================================

  void
  EventProcessor::terminateAbnormally_() try {
    if (ServiceRegistry::isAvailable<RandomNumberGenerator>()) {
      ServiceHandle<RandomNumberGenerator>()->saveToFile_();
    }
  }
  catch (...) {
  }

} // namespace art
