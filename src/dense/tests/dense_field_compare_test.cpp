#include "dense/ConfigFields.h"
#include "dense/ExternalSort.h"
#include "dense/Fusion.h"
#include "dense/Reconstruction.h"
#include "core/Sha256.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace {
namespace fs = std::filesystem;
using namespace spirula::dense;
using sfm::Vec3;
using Location = std::pair<uint32_t,uint32_t>;
struct Run {
    fs::path work;
    std::vector<uint64_t> counts;
    ReconstructionStatistics statistics;
    bool empty = false;
};

const JsonValue& required(const JsonValue& object, const char* key) {
    const auto* value = object.find(key);
    if (!value) throw std::runtime_error(std::string("missing diagnostic field: ") + key);
    return *value;
}

fs::path path(const fs::path& base, const JsonValue& object, const char* key) {
    const auto& value = required(object,key);
    if (value.type != JsonValue::Type::String || value.str.empty())
        throw std::runtime_error(std::string("invalid diagnostic path: ") + key);
    return fs::absolute(base / fs::u8path(value.str)).lexically_normal();
}

template<class T> std::vector<T> read(const fs::path& file, uint64_t count) {
    if (count > std::vector<T>().max_size() || fs::file_size(file) != count * sizeof(T))
        throw std::runtime_error("diagnostic array size mismatch: " + file.u8string());
    std::vector<T> values((size_t)count);
    std::ifstream input(file,std::ios::binary);
    input.read(reinterpret_cast<char*>(values.data()),(std::streamsize)(count * sizeof(T)));
    if (!input) throw std::runtime_error("cannot read diagnostic array: " + file.u8string());
    return values;
}

View source_view(const ParsedDataset& ds, size_t i) {
    View v; v.source_camera = true; v.source_image = (int64_t)i;
    v.camera.model = ds.camera_models.at(i); v.camera.tier = ds.camera_distortions.at(i);
    v.camera.width = ds.widths.at(i); v.camera.height = ds.heights.at(i);
    v.camera.fx = ds.intrins.at(4*i); v.camera.fy = ds.intrins.at(4*i+1);
    v.camera.cx = ds.intrins.at(4*i+2); v.camera.cy = ds.intrins.at(4*i+3);
    std::copy_n(ds.dist_coeffs.data()+8*i,8,v.camera.dist);
    if (!ds.redistort.empty()) {
        v.camera.source_model = ds.redistort.at(i).source_model;
        std::copy_n(ds.redistort.at(i).params,16,v.camera.source_params);
    }
    const float* pose = ds.c2w.data()+12*i;
    v.center = {pose[3],pose[7],pose[11]};
    for (int r=0;r<3;++r) for (int c=0;c<3;++c)
        v.world_to_camera[r*3+c] = pose[c*4+r] * (r==0 ? 1 : -1);
    v.validate(); return v;
}

ViewPixels pixels(const fs::path& base, const JsonValue& spec, const View& v, const DenseConfig& config) {
    ViewPixels out; out.width = v.camera.width; out.height = v.camera.height;
    const uint64_t n = (uint64_t)out.width*out.height;
    out.rgb = read<float>(path(base,spec,"rgb"),n*3);
    for (float value : out.rgb) if (!std::isfinite(value)) throw std::runtime_error("nonfinite diagnostic RGB");
    if (config.use_masks) {
        out.keep = read<uint8_t>(path(base,spec,"keep"),n);
        for (uint8_t value : out.keep) if (value>1) throw std::runtime_error("diagnostic keep mask must be binary");
    }
    return out;
}

spirula::roma::PairPrediction fields(const fs::path& directory, const std::string& prefix, const DenseConfig& config) {
    auto direction = [&](const char* name) {
        spirula::roma::Prediction out;
        out.width = config.match.high_width ? config.match.high_width : config.match.low_width;
        out.height = config.match.high_height ? config.match.high_height : config.match.low_height;
        const uint64_t n = (uint64_t)out.width*out.height;
        const auto stem = prefix + name + "_";
        out.warp = read<float>(directory/(stem+"warp.f32"),n*2);
        out.overlap = read<float>(directory/(stem+"overlap.f32"),n);
        out.precision = read<float>(directory/(stem+"precision.f32"),n*3);
        return out;
    };
    spirula::roma::PairPrediction out; out.forward = direction("AB");
    if (config.match.bidirectional) out.backward = direction("BA");
    return out;
}

Run reconstruct(const fs::path& root, const fs::path& base, const JsonValue& spec, const ParsedDataset& ds,
                const std::vector<View>& views, const DenseConfig& config, bool reference) {
    Run run; run.work = root/(reference ? "reference-work" : "native-work"); run.counts.resize(views.size());
    const auto preview = root/(reference ? "reference-preview" : "native-preview");
    Reconstruction core(run.work.u8string(),views,config,nullptr,preview.u8string());
    for (const auto& job : required(spec,"jobs").arr) {
        uint64_t ia=0,ib=0; assign_dense_value(ia,required(job,"a")); assign_dense_value(ib,required(job,"b"));
        if (ia>=views.size() || ib>=views.size()) throw std::runtime_error("diagnostic pair index is outside the view list");
        const auto a=(uint32_t)ia,b=(uint32_t)ib;
        const auto pa = pixels(base,required(spec,"views").arr.at(a),views.at(a),config);
        const auto pb = pixels(base,required(spec,"views").arr.at(b),views.at(b),config);
        const char* prefix_key=reference ? "reference_prefix" : "native_prefix";
        const auto* prefix=job.find(prefix_key);
        const std::string field_prefix=prefix ? prefix->as_string() : reference ? "reference_profile_" : "profile_";
        const auto directory=path(base,job,reference && job.has("reference_fields") ? "reference_fields" : "fields");
        const auto prediction = fields(directory,field_prefix,config);
        core.add_pair(a,b,pa,pb,prediction);
        if (config.match.bidirectional && job.find("complete_reverse") && job.find("complete_reverse")->as_bool())
            core.add_pair(b,a,pb,pa,{prediction.backward,prediction.forward});
    }
    uint64_t previous=0;
    for (uint32_t i=0;i<views.size();++i) {
        core.complete_reference(i);
        const auto checkpoint = core.checkpoint();
        if (!checkpoint.error.empty()) throw std::runtime_error(checkpoint.error);
        run.counts[i] = checkpoint.points-previous; previous = checkpoint.points;
    }
    try { run.statistics = core.finish((root/(reference ? "reference.ply" : "native.ply")).u8string(),ds); }
    catch (const std::exception& e) {
        if (previous || std::string(e.what()).find("dense filtering produced no points;") != 0) throw;
        run.empty = true;
    }
    return run;
}

using Points = std::map<Location,std::vector<Vec3>>;
Points accepted(const Run& run, const std::vector<View>& views, const DenseConfig& config) {
    Points points; std::ifstream input(run.work/"surfaces.bin",std::ios::binary);
    if (!input) throw std::runtime_error("cannot read diagnostic filtered surfaces");
    const int w = config.match.high_width ? config.match.high_width : config.match.low_width;
    const int h = config.match.high_height ? config.match.high_height : config.match.low_height;
    for (uint32_t i=0;i<views.size();++i) for (uint64_t j=0;j<run.counts[i];++j) {
        Surface surface;
        if (!read_disk_record(input,surface)) throw std::runtime_error("truncated diagnostic filtered surfaces");
        const Vec3 p{surface.point[0],surface.point[1],surface.point[2]}; sfm::Vec2 px;
        if (!project(views[i],p,px) || surface.support < (uint32_t)config.geometry.min_source_images)
            throw std::runtime_error("filtered diagnostic point lost source support or projection");
        const int x=(int)std::floor(px.x*w/views[i].camera.width), y=(int)std::floor(px.y*h/views[i].camera.height);
        if (x<0 || y<0 || x>=w || y>=h) throw std::runtime_error("filtered point is outside its reference grid");
        const sfm::Vec2 anchor{(x+0.5)*views[i].camera.width/w,(y+0.5)*views[i].camera.height/h};
        const auto residual=pixel_residual(views[i],px,anchor);
        if (std::hypot(residual.x,residual.y)>config.source_reprojection_error+1e-6)
            throw std::runtime_error("filtered point exceeds its reference reprojection limit");
        points[{i,(uint32_t)(y*w+x)}].push_back(p);
    }
    Surface extra; if (read_disk_record(input,extra)) throw std::runtime_error("unexpected diagnostic surface count");
    return points;
}

void distribution(JsonWriter& json, const char* name, std::vector<double> values) {
    json.key(name).object().field("count",(long long)values.size());
    if (!values.empty()) {
        double sum=0; for (double value : values) sum+=value*value;
        std::sort(values.begin(),values.end());
        json.field("rms",std::sqrt(sum/values.size())).field("median",values[values.size()/2]);
        json.field("p99",values[std::min(values.size()-1,(size_t)std::floor(values.size()*0.99))]).field("maximum",values.back());
    }
    json.end();
}

void statistics(JsonWriter& json, const char* name, const Run& run) {
    const auto& s=run.statistics;
    json.key(name).object().field("filtered",(long long)std::accumulate(run.counts.begin(),run.counts.end(),uint64_t{0}));
    json.field("empty",run.empty).field("reconstruction_statistics_available",!run.empty);
    if (!run.empty) {
        json.field("tested",(long long)s.tested).field("masked",(long long)s.masked).field("low_overlap",(long long)s.low_overlap).field("cycle",(long long)s.cycle);
        json.field("geometry",(long long)s.geometry).field("triangulated",(long long)s.triangulated).field("insufficient_support",(long long)s.insufficient_support);
        json.field("fused",(long long)s.fused).field("exported",(long long)s.exported).field("max_reference_reprojection_error",s.max_reference_reprojection_error);
        json.field("minimum_support",(long long)s.min_reference_support).field("maximum_support",(long long)s.max_reference_support);
        json.field("mean_support",s.refined ? (double)s.sum_reference_support/s.refined : 0);
        json.key("reprojection_histogram").array(); for (auto count : s.reference_reprojection_histogram) json.raw(std::to_string(count)); json.end();
    }
    json.end();
}
}

int main(int argc, char** argv) {
    try {
        if (argc!=4) throw std::runtime_error("usage: dense_field_compare_test DATASET SPEC_JSON NEW_OUTPUT_DIRECTORY");
        const fs::path dataset=fs::absolute(fs::u8path(argv[1])), spec_file=fs::absolute(fs::u8path(argv[2]));
        const fs::path output=fs::absolute(fs::u8path(argv[3]));
        if (fs::exists(output)) throw std::runtime_error("diagnostic output directory already exists");
        const auto spec=json_parse_file(spec_file.u8string());
        DenseConfig config; read_config(config,required(spec,"config"));
        if (config.matching_space!="source" || !required(spec,"views").is_array() || !required(spec,"jobs").is_array())
            throw std::runtime_error("diagnostic requires source configuration and view/job arrays");
        DatasetParserConfig parser; parser.require_image_files=false; parser.center_mode="camera-mean";
        const auto ds=parse_dataset(dataset.u8string(),parser,"");
        std::vector<View> views;
        for (const auto& v : required(spec,"views").arr) {
            const auto image=path(dataset/"images",v,"image");
            const auto found=std::find_if(ds.image_filenames.begin(),ds.image_filenames.end(),[&](const auto& candidate) {
                return fs::absolute(fs::u8path(candidate)).lexically_normal()==image;
            });
            if (found==ds.image_filenames.end()) throw std::runtime_error("diagnostic image is absent from the parsed reconstruction");
            views.push_back(source_view(ds,(size_t)(found-ds.image_filenames.begin())));
        }
        fs::create_directories(output);
        const auto native=reconstruct(output,spec_file.parent_path(),spec,ds,views,config,false);
        const auto reference=reconstruct(output,spec_file.parent_path(),spec,ds,views,config,true);
        const auto a=accepted(native,views,config), b=accepted(reference,views,config);
        uint64_t native_only=0, reference_only=0, common=0, unequal_clusters=0;
        std::vector<double> absolute, relative, projected;
        for (const auto& [key,points] : a) {
            const auto other=b.find(key);
            if (other==b.end()) { native_only+=points.size(); continue; }
            ++common; if (points.size()!=other->second.size()) ++unequal_clusters;
            std::set<size_t> used;
            for (const auto& p : points) {
                size_t index=other->second.size(); double nearest=std::numeric_limits<double>::infinity();
                for (size_t i=0;i<other->second.size();++i) if (!used.count(i)) {
                    const double distance=(p-other->second[i]).norm();
                    if (distance<nearest) { index=i; nearest=distance; }
                }
                if (index==other->second.size()) continue;
                used.insert(index); const auto& q=other->second[index];
                absolute.push_back(nearest); relative.push_back(nearest/(q-views[key.first].center).norm());
                double max_error=0;
                for (const auto& v : views) {
                    sfm::Vec2 pa,pb;
                    if (project(v,p,pa) && project(v,q,pb)) {
                        const auto delta=pixel_residual(v,pa,pb); max_error=std::max(max_error,std::hypot(delta.x,delta.y));
                    }
                }
                projected.push_back(max_error);
            }
            native_only += points.size()-used.size();
            reference_only += other->second.size()-used.size();
        }
        for (const auto& [key,points] : b) if (!a.count(key)) reference_only+=points.size();
        JsonWriter json; json.object().field("spec_sha256",spirula::sha256_file(spec_file.u8string()));
        json.field_raw("config",config_json(config)).field("pixel_frame","original image");
        statistics(json,"native",native); statistics(json,"reference",reference);
        json.field("common_reference_locations",(long long)common).field("native_only_points",(long long)native_only).field("reference_only_points",(long long)reference_only);
        json.field("locations_with_unequal_depth_clusters",(long long)unequal_clusters);
        json.field("projection_difference_scope","all supplied cameras where both points project");
        distribution(json,"common_point_distance_dataset_units",std::move(absolute));
        distribution(json,"common_point_distance_relative_to_reference_range",std::move(relative));
        distribution(json,"common_point_projection_difference_original_pixels",std::move(projected));
        if (const auto* plane=spec.find("expected_plane")) {
            const auto& n=required(*plane,"normal");
            if (!n.is_array() || n.arr.size()!=3 || required(*plane,"frame").as_string()!="centered reconstruction")
                throw std::runtime_error("expected plane requires a three-vector in the centered reconstruction frame");
            Vec3 normal{n.arr[0].as_double(),n.arr[1].as_double(),n.arr[2].as_double()};
            const double length=normal.norm(), offset=required(*plane,"offset").as_double()/length;
            if (!std::isfinite(length) || length<=0 || !std::isfinite(offset)) throw std::runtime_error("invalid expected plane");
            normal=normal*(1/length);
            auto errors=[&](const Points& points) {
                std::vector<double> values;
                for (const auto& entry : points) for (const auto& p : entry.second) values.push_back(std::fabs(p.dot(normal)-offset));
                return values;
            };
            json.key("expected_plane_before_fusion").object().field("frame","centered reconstruction");
            distribution(json,"native_distance_dataset_units",errors(a));
            distribution(json,"reference_distance_dataset_units",errors(b)); json.end();
        }
        json.end(); std::ofstream result(output/"comparison.json"); result<<json.str()<<'\n';
        if (!result) throw std::runtime_error("cannot write geometry comparison");
        std::printf("Completed shared-core geometry comparison: %llu common reference locations, %llu native-only and %llu reference-only points\n",
            (unsigned long long)common,(unsigned long long)native_only,(unsigned long long)reference_only);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
