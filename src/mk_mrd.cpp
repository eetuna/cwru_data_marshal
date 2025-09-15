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

#include <ismrmrd/dataset.h>
#include <ismrmrd/ismrmrd.h>
#include <complex>
#include <iostream>

int main(int argc, char **argv)
{
  const char *out = (argc > 1) ? argv[1] : "/data/mrd/minimal.h5";

  try
  {
    // Create/overwrite dataset (group name "dataset")
    ISMRMRD::Dataset d(out, "dataset", true);

    // Minimal ISMRMRD XML header (enough for tools to accept the file)
    const char *xml =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ismrmrdHeader xmlns="http://www.ismrm.org/ISMRMRD">
  <experimentalConditions>
    <H1resonanceFrequency_Hz>123000000</H1resonanceFrequency_Hz>
  </experimentalConditions>
  <encoding>
    <encodedSpace>
      <matrixSize><x>1</x><y>1</y><z>1</z></matrixSize>
      <fieldOfView_mm><x>1</x><y>1</y><z>1</z></fieldOfView_mm>
    </encodedSpace>
    <reconSpace>
      <matrixSize><x>1</x><y>1</y><z>1</z></matrixSize>
      <fieldOfView_mm><x>1</x><y>1</y><z>1</z></fieldOfView_mm>
    </reconSpace>
    <trajectory>cartesian</trajectory>
  </encoding>
</ismrmrdHeader>)";

    d.writeHeader(std::string(xml));

    // One tiny acquisition: 1 sample, 1 channel, 0 trajectory dims
    ISMRMRD::Acquisition acq;
    acq.resize(/*samples*/ 1, /*channels*/ 1, /*traj_dims*/ 0);
    acq.data(0, 0) = std::complex<float>(0.f, 0.f);
    d.appendAcquisition(acq);

    std::cout << "Wrote MRD to " << out << "\n";
    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "mk_mrd error: " << e.what() << "\n";
    return 1;
  }
}
