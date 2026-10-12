#pragma once

// What a reconstruction says while it runs, in the language the user picked.
//
// `spirula sfm` is not a third-party tool whose output we pass through: it is
// this program, run again as a child process, and the person reading its lines
// in the GUI's terminal is the same person who set the language. So every line
// a default run prints goes through here and comes out as
//
//     [tag] message
//
// with both halves translated. The tag names the stage; it is padded to a
// COMMON DISPLAY WIDTH across the tags of the current language, so the log
// stays a column even where a tag is two Han characters wide and its
// neighbour is seven Latin letters. Width is measured in terminal columns
// (East Asian wide characters count as two), not in bytes or codepoints.
//
// What does NOT come through here, on purpose: `--help` (documentation, and
// what a bug report quotes), the deep diagnostics behind SS_SFM_MAP_PROF and
// the audit / sub-model / bottom-up paths, and the self-test binaries. Those
// stay English -- they are read by whoever is debugging the pipeline, and a
// translated backtrace helps nobody.

#include "i18n/Message.h"

#include <functional>
#include <initializer_list>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#define SFM_LOG_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define SFM_LOG_PRINTF(a, b)
#endif

namespace sfm {
namespace slog {

// The stage a line came from. One per phase a user can name, not one per file.
enum class Tag {
    Run,       // the `auto` command itself: what it was asked for, and the summary
    Extract,   // feature detection, including the detector's own counts
    Match,     // matching, verification and the camera grouping they settle
    Map,       // the mapper and the passes that assemble its models
    Merge,     // merging models of a fragmented capture
    Orient,    // the final gauge fix (map/Orient.h)
    Device,    // which GPU, and what it can do
};

// Which stream the default sink writes a line to, and how a front end should
// style it. `Diag` is a developer diagnostic: English, carrying its own
// bracketed tag inside the text, never translated and never padded.
enum class Level { Info, Note, Warning, Error, Diag };

// Where lines go; the default prints them exactly as the CLI always has.
// Process-global: one SfM job per process. The text carries no tag prefix and
// no WARNING/ERROR word (Diag is verbatim). A sink may call prefix(); it must
// not log, which would deadlock on the lock ordering lines.
using Sink = std::function<void(Tag, Level, const std::string&)>;
void set_sink(Sink s);   // {} restores the printing default

// Also handed every line, after the sink or the terminal has it -- a copy for
// a watcher (app/SfmMonitor.h) that changes neither. Same rules as a sink.
using Tap = Sink;
void set_tap(Tap t);

// A line as the default sink prints it, without the newline.
std::string line(Tag t, Level lv, const std::string& text);

// "[extract] " -- localized, bracketed, and padded so every tag in the current
// language occupies the same number of terminal columns. By value because the
// cache behind it is rebuilt when the language changes, and the GUI reads this
// from its reconstruction thread while the printing happens on another.
std::string prefix(Tag t);

// Terminal columns a UTF-8 string occupies (East Asian wide = 2). Exposed
// because the summary block aligns its own labels the same way; the counting
// itself lives in i18n (i18n::display_width), since the CLI and the frame
// extractor lay out their own tables with it too.
int display_width(const char* s);

// One line to stdout / stderr, tagged and translated. `args` fills the
// message's {0}, {1}, ... placeholders.
void out(Tag t, const spirula::i18n::Msg& m,
         std::initializer_list<spirula::i18n::Arg> args = {});
void err(Tag t, const spirula::i18n::Msg& m,
         std::initializer_list<spirula::i18n::Arg> args = {});
// As err(), with the localized word for WARNING / ERROR in front of the
// message. Both go to stderr: a warning is not a result.
void warn(Tag t, const spirula::i18n::Msg& m,
          std::initializer_list<spirula::i18n::Arg> args = {});
void fail(Tag t, const spirula::i18n::Msg& m,
          std::initializer_list<spirula::i18n::Arg> args = {});

// A number rounded for display: num(1.14962, 2) -> "1.15". Needed wherever a
// printf said %.2f, because i18n::Arg's double conversion is %g and a median
// angle of 5.49856 degrees is not a thing anyone wanted to read.
std::string num(double v, int decimals);

// Text that is already formatted (a path, a number, a line built by the
// caller). Same tag column, no translation -- the `Raw` says so, as ui::*Raw
// does in the GUI.
void out_raw(Tag t, const std::string& text);
void err_raw(Tag t, const std::string& text);

// A developer diagnostic: printf-formatted, verbatim to stderr, no newline of
// its own. Write the bracketed tag into `fmt` -- these stay English and are
// not padded into the localized tag column.
void diag(Tag t, const char* fmt, ...) SFM_LOG_PRINTF(2, 3);

}  // namespace slog
}  // namespace sfm
