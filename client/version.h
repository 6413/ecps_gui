#pragma once

// The version this build says it is. "Check for updates" compares it with this same file on the main
// branch on GitHub, so a release is: bump this, push to main.
inline constexpr const char* ecps_version = "1.0.2";

// update test: this comment only exists to make a new commit, so "Check for updates" has something to find
// update test 2: another comment-only change, to get a newer commit than the one the installed build was made from

// Shown next to the version in the top bar. Change this text, push, press Update: if the new text appears the
// update worked. Leave it empty to show nothing.
inline constexpr const char* ecps_update_marker = "";
