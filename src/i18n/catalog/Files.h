#pragma once

// The Files window (app/gui/FileBrowser.h): every mounted storage, a folder's
// files, a look inside each, and moving files to and from another computer.
// English only so far (SS_MSG_EN); file names and sizes are never translated.

#include "i18n/BeginCatalog.h"

namespace spirula {
namespace i18n {
namespace msg {
namespace files {

SS_MSG_EN(window_title, "Files");
SS_MSG_EN(menu_browse, "Browse Files...");
SS_MSG_EN(menu_browse_help,
    "Every drive and volume this machine has mounted, its files, and a look inside "
    "each: pictures, videos, text, splats and point clouds. Runs keep going while it "
    "is open.");

SS_MSG_EN(locations, "Locations");
SS_MSG_EN(location_free, "{0} free of {1}");
SS_MSG_EN(back, "Back");
SS_MSG_EN(up, "Up");
SS_MSG_EN(refresh, "Refresh");
SS_MSG_EN(show_hidden, "Hidden files");
SS_MSG_EN(filter_hint, "Filter");
SS_MSG_EN(col_name, "Name");
SS_MSG_EN(col_size, "Size");
SS_MSG_EN(col_modified, "Modified");
SS_MSG_EN(reading, "Reading...");
SS_MSG_EN(empty_folder, "This folder is empty.");
SS_MSG_EN(cannot_read, "Cannot read this folder: {0}");
SS_MSG_EN(item_count, "Items: {0}");
SS_MSG_EN(nothing_selected, "Select a file to look inside it.");

SS_MSG_EN(kind_folder, "Folder");
SS_MSG_EN(kind_image, "Image");
SS_MSG_EN(kind_video, "Video");
SS_MSG_EN(kind_text, "Text");
SS_MSG_EN(kind_splat, "Splat / PLY");
SS_MSG_EN(kind_cloud, "Point cloud");
SS_MSG_EN(kind_mesh, "Mesh");
SS_MSG_EN(kind_archive, "Archive");
SS_MSG_EN(kind_other, "File");
SS_MSG_EN(size_modified, "{0}, modified {1}");

SS_MSG_EN(open_in_viewer, "Open in Spirula Viewer");
SS_MSG_EN(open_in_viewer_remote_help,
    "Opens it in a new browser tab on this computer, drawn by this computer's GPU. "
    "Nothing running on the server is touched.");
SS_MSG_EN(open_in_viewer_local_help,
    "Opens it in a separate Spirula Studio window, so whatever runs in this one "
    "carries on.");
SS_MSG_EN(open_here, "Open in Spirula Studio");
SS_MSG_EN(open_here_help, "Opens it in this window, as dropping it on the window would.");
SS_MSG_EN(preview_points, "Show points");
SS_MSG_EN(preview_points_help,
    "Draws its points here, in a small viewer of their own: the engine and any run "
    "are left alone.");
SS_MSG_EN(show_photos, "Show photos");
SS_MSG_EN(play_in_browser, "Play in browser");
SS_MSG_EN(play_in_browser_help,
    "Plays it in this computer's browser, which decodes it, if the browser knows the "
    "format (MP4/H.264 and WebM usually; .insv and .360 not).");
SS_MSG_EN(copy_path, "Copy path");

SS_MSG_EN(download, "Download");
SS_MSG_EN(download_help,
    "Saves it on this computer through the browser. A folder comes as one .tar file.");
SS_MSG_EN(upload_here, "Upload here...");
SS_MSG_EN(upload_help,
    "Choose files or a whole folder on this computer to copy into this folder. Files "
    "dragged onto the page land here too, while this window is open.");
SS_MSG_EN(export_to, "Export...");
SS_MSG_EN(export_help, "Copies it into a folder of your choice.");
SS_MSG_EN(import_files, "Import files...");
SS_MSG_EN(import_folder, "Import folder...");
SS_MSG_EN(import_help, "Copies files or a folder from elsewhere into this folder.");
SS_MSG_EN(pick_export, "Export to");
SS_MSG_EN(pick_import_files, "Files to import");
SS_MSG_EN(pick_import_folder, "Folder to import");
SS_MSG_EN(copying, "Copying {0}...");
SS_MSG_EN(copy_done, "Copied {0} ({1}).");
SS_MSG_EN(copy_failed, "Copy failed: {0}");

SS_MSG_EN(new_folder, "New folder");
SS_MSG_EN(new_folder_name, "Name");
SS_MSG_EN(create, "Create");
SS_MSG_EN(cancel, "Cancel");
SS_MSG_EN(create_failed, "Cannot create it: {0}");

SS_MSG_EN(ply_splat, "Gaussian splat. Splats: {0}, SH degree: {1}");
SS_MSG_EN(ply_mesh, "Mesh. Vertices: {0}, faces: {1}");
SS_MSG_EN(ply_points, "Point cloud. Points: {0}");
SS_MSG_EN(ply_unreadable, "Not a PLY file this can read.");
SS_MSG_EN(ply_properties, "Properties");
SS_MSG_EN(text_binary, "Not a text file.");
SS_MSG_EN(text_truncated, "Showing the first {0}.");
SS_MSG_EN(image_failed, "Cannot show this image.");

}  // namespace files
}  // namespace msg
}  // namespace i18n
}  // namespace spirula

#include "i18n/EndCatalog.h"
