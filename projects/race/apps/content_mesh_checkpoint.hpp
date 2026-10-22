#pragma once
#include "content_project.hpp"
#include "mesh_checkpoint.hpp"

namespace race::mesh_checkpoint {
// The admitted typed owner retains its complete raw index and recipe closure.
// Non-mesh selection fails before product IO; checkpoint wire bytes stay schema 1.
Result<Published> capture(const content_project::Snapshot&,assets::mesh::Lease,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints,const assets::mesh::Limits& = {});
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,
    const content_project::Snapshot&,platform::RootedFiles& products,const assets::mesh::Limits& = {});
} // namespace race::mesh_checkpoint
