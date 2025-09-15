#include <catch2/catch_all.hpp>
#include "common/pose.hpp"


TEST_CASE("pose store roundtrip"){
PoseStore s; Pose p; p.p={1,2,3}; s.set(p); auto q=s.get();
REQUIRE(q.p[0]==1); REQUIRE(q.p[1]==2); REQUIRE(q.p[2]==3);
}