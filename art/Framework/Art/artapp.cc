#include "art/Framework/Art/artapp.h"
// vim: set sw=2 expandtab :

#include "art/Framework/Art/BasicOptionsHandler.h"
#include "art/Framework/Art/BasicOutputOptionsHandler.h"
#include "art/Framework/Art/BasicPostProcessor.h"
#include "art/Framework/Art/BasicSourceOptionsHandler.h"
#include "art/Framework/Art/DebugOptionsHandler.h"
#include "art/Framework/Art/FileCatalogOptionsHandler.h"
#include "art/Framework/Art/OptionsHandlers.h"
#include "art/Framework/Art/ProcessingOptionsHandler.h"
#include "art/Framework/Art/run_art.h"
#include "cetlib/filepath_maker.h"

#include "boost/filesystem.hpp"
#include "boost/program_options.hpp"

#include <iostream>
#include <string>

namespace bpo = boost::program_options;

using namespace std;

int
artapp(int argc, char* argv[], bool report_unused)
{
  // Configuration file lookup policy.
  string search_path;
  if (char const* fhicl_env = std::getenv("FHICL_FILE_PATH")) {
    search_path = string{fhicl_env};
  } else {
    cerr << "Expected environment variable FHICL_FILE_PATH is missing or "
            "empty: using \".\"\n";
    search_path = ".";
  }
  cet::filepath_first_absolute_or_lookup_with_dot lookupPolicy{search_path};

  // Create and store options handlers.
  ostringstream descstr;
  descstr << "\nUsage: " << boost::filesystem::path(argv[0]).filename().native()
          << " <-c <config-file>> <other-options> [<source-file>]+\n\n"
          << "Basic options";
  bpo::options_description all_desc{descstr.str()};
  art::OptionsHandlers handlers;
  handlers.reserve(7);
  // BasicOptionsHandler should always be first in the list!
  handlers.emplace_back(
    new art::BasicOptionsHandler{all_desc, lookupPolicy, report_unused});
  // Additional options
  handlers.emplace_back(new art::BasicSourceOptionsHandler{all_desc});
  handlers.emplace_back(new art::BasicOutputOptionsHandler{all_desc});
  handlers.emplace_back(new art::ProcessingOptionsHandler{all_desc});
  handlers.emplace_back(new art::DebugOptionsHandler{all_desc});
  handlers.emplace_back(new art::FileCatalogOptionsHandler{all_desc});
  // BasicPostProcessor should be last.
  handlers.emplace_back(new art::BasicPostProcessor);
  return art::run_art(argc, argv, all_desc, move(handlers));
}

// Local Variables:
// mode: c++
// End:
