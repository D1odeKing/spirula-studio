#pragma once
// Which licences the user has accepted, and the full terms of the ones we show
// verbatim. The record is the `accepted_license=<family>` lines of the GUI's
// gui.conf, so the GUI, the CLI and a child process the GUI launches all read
// one answer. Nothing here downloads anything.

#include <string>
#include <vector>

namespace spirula::license {

// A licence displayed in full before a download. `text` is the agreement as
// published, byte for byte (LICENSES/), for offline display.
struct Terms {
    const char* family;   // "sam3", "sam2", "gdino", "birefnet", or a registered one
    const char* title;    // English, for the terminal; the GUI uses its Msg
    const char* url;      // the current text, which may be newer than `text`
    const char* text;
};

// Null for a family that is not known. Every family we gate has its terms
// embedded, so every consent dialog and terminal prompt shows the whole text.
// SAM 3, SAM 2.1, Grounding DINO and BiRefNet are built in.
const Terms* terms_for(const std::string& family);

// Adds a family to the table terms_for() reads. A feature calls this at startup,
// before any thread asks; `t` and its strings must outlive the process. Idempotent
// per family.
void register_terms(const Terms* t);

// Every family terms_for() knows, built in first and then in registration
// order, comma separated: what an "unknown licence" error names as the choices.
std::string known_families();

// A comma list split and trimmed, empty items dropped: "sam3, gdino" is {sam3, gdino}.
std::vector<std::string> split_families(const std::string& list);

// A name for a file written beside `path` and renamed over it. Different for every
// caller, so two writers (the GUI and a CLI) cannot trample each other's half-written copy.
std::string scratch_path_for(const std::string& path);

// <config>/gui.conf, where app::config_dir() puts it. Empty when neither
// XDG_CONFIG_HOME nor HOME is set (APPDATA on Windows): app::config_dir() falls
// back to the working directory there, and consent read from there is not consent.
std::string settings_path();

bool accepted(const std::string& family);
// Idempotent; every other line of the file is kept as it is. False when the
// family is not known, or the file could not be written: it is NOT accepted then.
bool record(const std::string& family);
std::vector<std::string> accepted_all();
// The families of a comma list that are not accepted, in order. Empty means go.
std::vector<std::string> missing(const std::string& families);

// The families named by several comma lists, each once, in first-seen order: what
// a batch asks for up front, whatever number of files name the same licence.
std::vector<std::string> unique_families(const std::vector<std::string>& lists);

// Whether a consent dialog may enable its Accept button. Every family needs the
// "I have read and accept" tick, none is exempt (the SAM and Grounding DINO
// licences included), and a family we hold no terms for cannot be accepted at all.
bool accept_enabled(const std::string& family, bool ticked);

}  // namespace spirula::license
