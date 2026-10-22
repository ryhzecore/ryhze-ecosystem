#include "assets_support.hpp"
#include <cstring>
void asset_mesh_tests();
void mesh_pipeline_tests();
void mesh_render_tests();
void asset_package_tests();
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (!std::strcmp(argv[1], "wire"))
    asset_wire_tests();
  else if (!std::strcmp(argv[1], "runtime"))
    asset_runtime_tests();
  else if (!std::strcmp(argv[1], "faults"))
    asset_fault_tests();
  else if (!std::strcmp(argv[1], "mesh"))
    asset_mesh_tests();
  else if (!std::strcmp(argv[1], "import"))
    mesh_pipeline_tests();
  else if (!std::strcmp(argv[1], "mesh_render"))
    mesh_render_tests();
  else if (!std::strcmp(argv[1], "package"))
    asset_package_tests();
  else
    return 2;
  std::printf("assets %s passed\n", argv[1]);
}
