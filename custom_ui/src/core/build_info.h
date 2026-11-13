// Build identification shown in Settings -> System Information.
#pragma once

namespace core {

// `git describe --always --dirty` of the tree custom_ui was built from
// (generated into build/git_rev.h by the Makefile), or "unknown".
const char * build_git_rev();

}  // namespace core
