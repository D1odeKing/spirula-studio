#pragma once

// FeatureWatcher -- the frames of a run, with the features found on them, while
// the extractor is still working through the rest.
//
// It puts each image on the reel as its feature file appears, so the reel
// advances at the pace of the stage, in the extractor's order, whatever that
// is. Its own thread because it decodes an image per frame, which is not
// something to do between two ImGui calls.
//
// No channel from the child is involved: the feature files ARE the channel,
// and they are written whether or not anybody is watching.

#include "app/gui/FilmReel.h"

#include <atomic>
#include <string>
#include <thread>

namespace gui {

class FeatureWatcher {
public:
    ~FeatureWatcher();

    // Idempotent for the same folders: starting it again on the ones it is
    // already watching does nothing, which is what lets the screen call it
    // every frame the step is running. `mask_dir` may be empty.
    // `thumb_dir` is the run's thumbs/ (sfm/core/Progress.h); empty falls back
    // to decoding the source image, which is what a finished run has left.
    void start(const std::string& image_dir, const std::string& mask_dir,
               const std::string& features_dir, FilmReel* film,
               const std::string& thumb_dir = {});
    void stop();

private:
    void run(std::string image_dir, std::string mask_dir,
             std::string features_dir, FilmReel* film, std::string thumb_dir);

    std::thread _worker;
    std::atomic<bool> _stop{false};
    std::string _image_dir, _mask_dir, _features_dir, _thumb_dir;
};

}  // namespace gui
