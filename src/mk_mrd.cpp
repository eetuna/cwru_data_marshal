/*
 * File: src/mk_mrd.cpp
 * Project: CWRU Data Marshal
 * Purpose: Internal support module
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#include <iostream>
#include <string>
#include "mk_mrd_utils.hpp"

int main(int argc, char **argv)
{
  const char *out = (argc > 1) ? argv[1] : "./data/mrd/minimal.h5";

  try
  {
    generate_minimal_mrd(out);
    std::cout << "Wrote MRD to " << out << "\n";
    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "mk_mrd error: " << e.what() << "\n";
    return 1;
  }
}
