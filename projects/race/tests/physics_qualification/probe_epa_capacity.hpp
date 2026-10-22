// Public fixed-pool capacity corroboration, not a physical maximum-depth case.
namespace epa_capacity_probe {
void run() {
  using Builder=JPH::EPAConvexHullBuilder;
  static_assert(Builder::cMaxTriangles==256&&Builder::cMaxPoints==128&&Builder::cMaxEdgeLength==256);
  struct StackEntryLayout {Builder::Triangle* triangle;int edge;int iteration;};
  static_assert(sizeof(StackEntryLayout)==16);
  const auto heap_before=sdk_arena->stats.live_bytes,crt_before=crt_requests.load();
  Builder::TriangleFactory factory;
  const JPH::Vec3 points[]{JPH::Vec3(1,0,0),JPH::Vec3(0,1,0),JPH::Vec3(0,0,1)};
  std::array<Builder::Triangle*,Builder::cMaxTriangles> triangles{};
  for(std::size_t i=0;i<triangles.size();++i){
    triangles[i]=factory.CreateTriangle(0,1,2,points);
    require(triangles[i]!=nullptr,"EPA factory accepts exact pool capacity");
    require(std::find(triangles.begin(),triangles.begin()+i,triangles[i])==triangles.begin()+i,"EPA live triangle identities are distinct");
  }
  require(factory.CreateTriangle(0,1,2,points)==nullptr,"EPA pool exhaustion returns null");
  Builder::Edges edges;Builder::NewTriangles created;
  for(std::size_t i=0;i<triangles.size();++i){
    edges.push_back(Builder::Edge{triangles[i],int(i%3),int(i%128)});
    created.push_back(triangles[i]);
  }
  require(edges.size()==edges.capacity()&&created.size()==created.capacity(),"both EPA output containers reach declared capacity");
  for(std::size_t i=0;i<triangles.size();++i)
    require(edges[unsigned(i)].mNeighbourTriangle==triangles[i]&&edges[unsigned(i)].mNeighbourEdge==int(i%3)&&
            edges[unsigned(i)].mStartIdx==int(i%128)&&created[unsigned(i)]==triangles[i],"EPA fixed-container values preserved");
  edges.clear();created.clear();
  require(edges.empty()&&created.empty(),"EPA fixed-container teardown");
  auto* retired=triangles.back();factory.FreeTriangle(retired);
  auto* reused=factory.CreateTriangle(0,1,2,points);
  require(reused==retired&&!reused->mRemoved,"EPA reuse follows explicit free and construction");
  for(auto* triangle:triangles)factory.FreeTriangle(triangle);
  require(sdk_arena->stats.live_bytes==heap_before&&crt_requests.load()==crt_before,"EPA pool uses no heap allocation");
  std::printf("{\"epa_capacity_passed\":true,\"synthetic_capacity_fixture\":true,\"physical_depth_reproduction\":false,\"triangles\":256,\"points\":128,\"edges\":256,\"new_triangles\":256,\"entry_layout_bytes\":16,\"derived_array_bytes\":4096,\"horizon_array_delta_bytes\":3072,\"edges_container_bytes\":%zu,\"new_triangles_container_bytes\":%zu,\"factory_bytes\":%zu,\"builder_bytes\":%zu,\"allocation_proved\":false}\n",sizeof(edges),sizeof(created),sizeof(factory),sizeof(Builder));
}
}
