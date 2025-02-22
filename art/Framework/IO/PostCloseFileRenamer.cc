#include "art/Framework/IO/PostCloseFileRenamer.h"

#include "art/Framework/IO/FileStatsCollector.h"
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/filesystem.hpp"
#include "canvas/Utilities/Exception.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace bfs = boost::filesystem;

// For sanity.  To avoid unintentionally introducing std overloads, we
// do not make a using declaration for boost::regex.
using namespace boost::regex_constants;
using boost::regex_match;
using boost::regex_search;
using boost::smatch;

namespace {
  boost::regex const rename_re{
    "%[lp]|%(\\d+)?([#rRsS])|%t([ocrRsS])|%if([bnedp])|%"
    "ifs%([^%]*)%([^%]*)%([ig]*)%|%.",
    ECMAScript};

  std::string
  to_string(boost::posix_time::ptime const& pt)
  {
    if (pt == boost::posix_time::ptime{}) {
      return "-";
    }
    return boost::posix_time::to_iso_string(pt);
  }
}

art::PostCloseFileRenamer::PostCloseFileRenamer(FileStatsCollector const& stats)
  : stats_{stats}
{}

// Do not substitute for the "%(\\d+)?#" pattern here.
std::string
art::PostCloseFileRenamer::applySubstitutionsNoIndex_(
  std::string const& filePattern) const
{
  std::string result; // Empty
  smatch match;
  auto sb = cbegin(filePattern), sid = sb, se = cend(filePattern);
  while (regex_search(sid, se, match, rename_re)) {
    // Precondition: that the regex creates the sub-matches we think it
    // should.
    assert(match.size() == 8);
    // Subexpressions:
    //   0: Entire matched expression.
    //   1. Possible fill format digits for numeric substitution.
    //   2. Numeric substitution specifier.
    //   3. Timestamp substitution specifier.
    //   4. Input file name substitution specifier.
    //   5. Input file name regex match clause.
    //   6. Input file name regex substitution clause.
    //   7. Input file name regex flags.
    //
    // Note that we're not using named capture groups because that is
    // not supported by C++11 to which we will eventually migrate when
    // GCC supports the full C++11 regex functionality (reportedly
    // 4.9.0).
    //
    // Add the bit before the next substitution pattern to the result.
    result += match.prefix();
    // Decide what we're going to add to the result instead of the
    // substitution pattern:
    switch (*(match[0].first + 1)) {
    case 'l':
      result += stats_.moduleLabel();
      break;
    case 'p':
      result += stats_.processName();
      break;
    case 'i':
      result += subInputFileName_(match);
      break;
    case 't':
      result += subTimestamp_(match);
      break;
    default:
      if (match[2].matched) { // [#rRsS]
        result += subFilledNumericNoIndex_(match);
      } else {
        throw Exception(errors::Configuration)
          << "Unrecognized substitution %" << *(match[0].first + 1)
          << " in pattern " << filePattern
          << " -- typo or misconstructed regex?\n";
      }
      break;
    }
    sid = match[0].second; // Set position for next match start.
  }
  result.append(sid, se); // Append unmatched text at end.
  return result;
}

std::string
art::PostCloseFileRenamer::subInputFileName_(boost::smatch const& match) const
{
  std::string result;
  // If the filename is empty, substitute "-". If it is merely the
  // required substitution that is empty, substitute "".
  if (!stats_.lastOpenedInputFile().empty()) {
    bfs::path const ifp{stats_.lastOpenedInputFile()};
    if (match[4].matched) { // %if[bdenp]
      switch (*(match[4].first)) {
      case 'b': {
        // Base name without extension.
        result = ifp.stem().native();
      } break;
      case 'd': {
        // Fully resolved path without filename.
        result = canonical(ifp.parent_path()).native();
      } break;
      case 'e': {
        // Extension.
        result = ifp.extension().native();
      } break;
      case 'n': {
        // Base name with extension.
        result = ifp.filename().native();
      } break;
      case 'p': {
        // Fully-resolved path with filename.
        result = (canonical(ifp.parent_path()) / ifp.filename()).native();
      } break;
      default: // INTERNAL_ERROR.
        Exception(errors::LogicError)
          << "Internal error: subInputFileName_() did not recognize "
             "substitution code %if"
          << *(match[4].first) << ".\n";
        break;
      }
    } else if (match[5].matched) { // Regex substitution.
      // Decompose the regex;
      syntax_option_type sflags{ECMAScript};
      match_flag_type mflags{match_default};
      bool global{false};
      if (match[7].matched) { // Options.
        for (auto c : match[7].str()) {
          switch (c) {
          case 'i':
            sflags |= icase;
            break;
          case 'g':
            global = true;
            break;
          default: // INTERNAL ERROR.
            throw Exception(errors::LogicError)
              << "Internal error: subInputFileName_() did not recognize "
                 "regex flag '"
              << c << "'.\n";
            break;
          }
        }
      }
      if (!global) {
        mflags |= format_first_only;
      }
      boost::regex const dsub{match[5].str(), sflags};
      result = regex_replace(
        stats_.lastOpenedInputFile(), dsub, match[6].str(), mflags);
    } else { // INTERNAL ERROR.
      throw Exception(errors::LogicError)
        << "Internal error: subInputFileName_() called for unknown reasons "
           "with pattern "
        << match[0].str() << ".\n";
    }
  } else {
    result = "-";
  }
  return result;
}

std::string
art::PostCloseFileRenamer::subTimestamp_(boost::smatch const& match) const
{
  std::string result;
  switch (*(match[3].first)) {
  case 'o': // Open.
    result = to_string(stats_.outputFileOpenTime());
    break;
  case 'c': // Close.
    result = to_string(stats_.outputFileCloseTime());
    break;
  case 'r': // Start of Run with lowest number.
    result = to_string(stats_.lowestRunStartTime());
    break;
  case 'R': // Start of Run with highest number.
    result = to_string(stats_.highestRunStartTime());
    break;
  case 's': // Start of SubRun with lowest number.
    result = to_string(stats_.lowestSubRunStartTime());
    break;
  case 'S': // Start of SubRun with highest number.
    result = to_string(stats_.highestSubRunStartTime());
    break;
  default: // INTERNAL ERROR.
    throw Exception(errors::LogicError)
      << "Internal error: subTimestamp_() did not recognize substitution "
         "code %t"
      << *(match[3].first) << ".\n";
    break;
  }
  return result;
}

std::string
art::PostCloseFileRenamer::subFilledNumericNoIndex_(
  boost::smatch const& match) const
{
  bool const did_match = match[1].matched;
  std::string const format_string = match[1].str();
  auto zero_fill = [did_match, &format_string](std::ostringstream& os,
                                               auto const& num) {
    if (did_match) {
      os << std::setfill('0') << std::setw(std::stoul(format_string));
    }
    os << num;
  };

  std::string result;
  std::ostringstream num_str;

  switch (*(match[2].first)) {
  case '#':
    // In order to get the indexing correct, we cannot yet
    // substitute the index.  We must wait until the entire filename
    // as been assembled, with all other substitutions evaluated.
    // At this point, we need to restore the original pattern.
    num_str << "%" << match[1].str() << "#";
    break;
  case 'r':
    if (stats_.lowestRunID().isValid()) {
      zero_fill(num_str, stats_.lowestRunID().run());
    }
    break;
  case 'R':
    if (stats_.highestRunID().isValid()) {
      zero_fill(num_str, stats_.highestRunID().run());
    }
    break;
  case 's':
    if (stats_.lowestSubRunID().isValid()) {
      zero_fill(num_str, stats_.lowestSubRunID().subRun());
    }
    break;
  case 'S':
    if (stats_.highestSubRunID().isValid()) {
      zero_fill(num_str, stats_.highestSubRunID().subRun());
    }
    break;
  default: // INTERNAL ERROR.
    break;
  }
  result = num_str.str();
  if (result.empty()) {
    result = "-";
  }
  return result;
}

std::string
art::PostCloseFileRenamer::applySubstitutions(std::string const& toPattern)
{
  std::string const finalPattern = applySubstitutionsNoIndex_(toPattern);
  auto const components = detail::componentsFromPattern(finalPattern);

  // Get index for the processed pattern, incrementing if a file-close
  // has been recorded.
  auto& index = indexForProcessedPattern_[components];
  if (stats_.fileCloseRecorded()) {
    ++index;
  }

  return components.fileNameWithIndex(index);
}

std::string
art::PostCloseFileRenamer::maybeRenameFile(std::string const& inPath,
                                           std::string const& toPattern)
{
  std::string const& newFile = applySubstitutions(toPattern);
  boost::system::error_code ec;
  bfs::rename(inPath, newFile, ec);
  if (ec) {
    // Fail (different filesystems? Try copy / delete instead).
    // This attempt will throw on failure.
    bfs::copy_file(inPath, newFile, bfs::copy_option::overwrite_if_exists);
    bfs::remove(inPath);
  }
  return newFile;
}
