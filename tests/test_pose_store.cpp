/*
 * File: tests/test_pose_store.cpp
 * Project: CWRU Data Marshal
 * Purpose: Internal support module
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#include <catch2/catch_all.hpp>
#include "common/pose.hpp"


TEST_CASE("pose store roundtrip"){
PoseStore s; Pose p; p.p={1,2,3}; s.set(p); auto q=s.get();
REQUIRE(q.p[0]==1); REQUIRE(q.p[1]==2); REQUIRE(q.p[2]==3);
}