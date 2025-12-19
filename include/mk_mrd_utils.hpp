#pragma once

#include <ismrmrd/dataset.h>
#include <ismrmrd/ismrmrd.h>
#include <complex>
#include <string>

// Generates a minimal valid MRD file at the given path
inline void generate_minimal_mrd(const std::string& out)
{
    // Create/overwrite dataset (group name "dataset")
    ISMRMRD::Dataset d(out.c_str(), "dataset", true);

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
}
