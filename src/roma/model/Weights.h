#pragma once

#include "nn/WeightStore.h"
#include "nn/io/TorchCheckpoint.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spirula::roma {

class Weights {
public:
    // `progress(done, total)` counts checkpoint tensors.
    void load(const std::string& path, bool mixed = false,
              const std::function<void(uint64_t, uint64_t)>& progress = {});
    bool mixedPrecision() const { return mixed_; }
    void release() { store_.release(); }
    nn::Tensor get(const std::string& name) const { return store_.get(name); }
    bool has(const std::string& name) const { return store_.has(name); }
    uint64_t bytes() const { return store_.deviceBytes(); }
    const std::vector<float>& descriptorPeriods() const { return descriptor_periods_; }
    const std::vector<float>& matcherPeriods() const { return matcher_periods_; }
    const std::vector<float>& matcherOmega() const { return matcher_omega_; }
    float matcherScale() const { return matcher_scale_; }
    float matcherTemperature() const { return matcher_temperature_; }
    static void validate(const nn::TorchCheckpoint& checkpoint);

private:
    bool mixed_ = false;
    nn::WeightStore store_;
    std::vector<float> descriptor_periods_, matcher_periods_;
    std::vector<float> matcher_omega_;
    float matcher_scale_ = 0, matcher_temperature_ = 0;
};

}  // namespace spirula::roma
