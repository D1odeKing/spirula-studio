#pragma once

// The ruler on the trainer's 3D view: two picked points, their distance in the
// dataset's FILE frame, and "Set true length", which rescales the dataset to
// metres with rescale_dataset() (data/DatasetRescale.h, docs/meshlab.md).

#include "app/gui/ViewportInput.h"

#include <future>
#include <string>
#include <vector>

struct ParsedDataset;

namespace gui {

class ViewportPanel;

class MeasureTool : public ViewportInteractor {
public:
    // One toolbar slot over `panel`, which shows `ds` (null before a parse)
    // parsed from `dataset`. True once a rescale has finished: re-parse.
    bool draw_toolbar(ViewportPanel& panel, const ParsedDataset* ds,
                      const std::string& dataset, bool can_rescale);
    // Lines for the log panel since the last call.
    std::vector<std::string> take_log();

    bool owns_left_button() const override { return _on; }
    bool on_viewport_input(const ViewportInput& in) override;
    void draw_viewport_overlay(const ViewportOverlay& v) override;

private:
    double length() const;   // file-frame units; < 0 without two points
    void draw_confirm();

    ViewportPanel* _panel = nullptr;
    const ParsedDataset* _ds = nullptr;
    std::string _dataset;
    bool _on = false;
    bool _waiting = false;               // a pick is on its way back
    int _count = 0;
    float _pts[2][3] = {};               // the panel's MODEL frame (normalized)
    double _truth = 0.0;                 // metres
    double _scale = 1.0;                 // what the confirmation applies
    bool _confirm_open = false, _confirm_shown = false;
    std::future<std::string> _job;       // "" on success, else the error
    std::vector<std::string> _log;
};

}  // namespace gui
