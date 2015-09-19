#include "VdbGrid.hpp"

#if OPENVDB_AVAILABLE

#include "VdbRaymarcher.hpp"

#include "sampling/PathSampleGenerator.hpp"

#include "io/JsonUtils.hpp"
#include "io/Scene.hpp"

#include "Debug.hpp"

#include <openvdb/tools/Interpolation.h>
#include <iostream>

namespace Tungsten {

std::string VdbGrid::sampleMethodToString(SampleMethod method)
{
    switch (method) {
    case SampleMethod::Raymarching:  return "raymarching";
    default:
    case SampleMethod::ExactNearest: return "exact_nearest";
    }
}

std::string VdbGrid::integrationMethodToString(IntegrationMethod method)
{
    switch (method) {
    case IntegrationMethod::Raymarching:  return "raymarching";
    default:
    case IntegrationMethod::ExactNearest: return "exact_nearest";
    }
}

VdbGrid::SampleMethod VdbGrid::stringToSampleMethod(const std::string &name)
{
    if (name == "exact_nearest")
        return SampleMethod::ExactNearest;
    else if (name == "raymarching")
        return SampleMethod::Raymarching;
    FAIL("Invalid sample method: '%s'", name);
}

VdbGrid::IntegrationMethod VdbGrid::stringToIntegrationMethod(const std::string &name)
{
    if (name == "exact_nearest")
        return IntegrationMethod::ExactNearest;
    else if (name == "raymarching")
        return IntegrationMethod::Raymarching;
    FAIL("Invalid integration method: '%s'", name);
}

VdbGrid::VdbGrid()
: _gridName("density"),
  _integrationString("exact_nearest"),
  _sampleString("exact_nearest"),
  _stepSize(5.0f)
{
    _integrationMethod = stringToIntegrationMethod(_integrationString);
    _sampleMethod = stringToSampleMethod(_sampleString);
}

void VdbGrid::fromJson(const rapidjson::Value &v, const Scene &scene)
{
    _path = scene.fetchResource(v, "file");
    JsonUtils::fromJson(v, "grid_name", _gridName);
    JsonUtils::fromJson(v, "integration_method", _integrationString);
    JsonUtils::fromJson(v, "sampling_method", _sampleString);
    JsonUtils::fromJson(v, "step_size", _stepSize);
    JsonUtils::fromJson(v, "transform", _configTransform);

    _integrationMethod = stringToIntegrationMethod(_integrationString);
    _sampleMethod = stringToSampleMethod(_sampleString);
}

rapidjson::Value VdbGrid::toJson(Allocator &allocator) const
{
    rapidjson::Value v = Grid::toJson(allocator);

    v.AddMember("type", "vdb", allocator);
    v.AddMember("file", _path->asString().c_str(), allocator);
    v.AddMember("grid_name", _gridName.c_str(), allocator);
    v.AddMember("integration_method", _integrationString.c_str(), allocator);
    v.AddMember("sampling_method", _sampleString.c_str(), allocator);
    if (_integrationMethod == IntegrationMethod::Raymarching || _sampleMethod == SampleMethod::Raymarching)
        v.AddMember("step_size", _stepSize, allocator);
    v.AddMember("transform", JsonUtils::toJsonValue(_configTransform, allocator), allocator);

    return std::move(v);
}

void VdbGrid::loadResources()
{
    openvdb::io::File file(_path->absolute().asString());
    try {
        file.open();
    } catch(const openvdb::IoError &e) {
        FAIL("Failed to open vdb file at '%s': %s", *_path, e.what());
    }

    openvdb::GridBase::Ptr ptr = file.readGrid(_gridName);
    if (!ptr)
        FAIL("Failed to read grid '%s' from vdb file '%s'", _gridName, *_path);

    file.close();

    _grid = openvdb::gridPtrCast<openvdb::FloatGrid>(ptr);
    if (!_grid)
        FAIL("Failed to read grid '%s' from vdb file '%s': Grid is not a FloatGrid", _gridName, *_path);

    openvdb::CoordBBox bbox = _grid->evalActiveVoxelBoundingBox();
    Vec3i minP = Vec3i(bbox.min().x(), bbox.min().y(), bbox.min().z());
    Vec3i maxP = Vec3i(bbox.max().x(), bbox.max().y(), bbox.max().z());
    Vec3f diag = Vec3f(maxP - minP);
    float scale = 1.0f/diag.max();
    diag *= scale;
    Vec3f center = Vec3f(minP)*scale + Vec3f(diag.x(), 0.0f, diag.z())*0.5f;

    std::cout << minP << " -> " << maxP << std::endl;

    _transform = Mat4f::translate(-center)*Mat4f::scale(Vec3f(scale));
    _invTransform = Mat4f::scale(Vec3f(1.0f/scale))*Mat4f::translate(center);
    _bounds = Box3f(Vec3f(minP), Vec3f(maxP));

    _invConfigTransform = _configTransform.invert();
}

Mat4f VdbGrid::naturalTransform() const
{
    return _configTransform*_transform;
}

Mat4f VdbGrid::invNaturalTransform() const
{
    return _invTransform*_invConfigTransform;
}

Box3f VdbGrid::bounds() const
{
    return _bounds;
}

static inline float gridAt(const openvdb::FloatGrid::Ptr &grid, Vec3f p)
{
    return openvdb::tools::BoxSampler::sample(grid->tree(), openvdb::Vec3R(p.x(), p.y(), p.z()));
}

float VdbGrid::density(Vec3f p) const
{
    return gridAt(_grid, p);
}

float VdbGrid::densityIntegral(PathSampleGenerator &sampler, Vec3f p, Vec3f w, float t0, float t1) const
{
    if (_integrationMethod == IntegrationMethod::ExactNearest) {
        VdbRaymarcher<openvdb::FloatGrid::TreeType, 3> dda;
        auto accessor = _grid->getConstAccessor();

        float result = 0.0f;
        dda.march(DdaRay(p + 0.5f, w), t0, t1, accessor, [&](openvdb::Coord voxel, float ta, float tb) {
            result += accessor.getValue(voxel)*(tb - ta);
            return false;
        });
        return result;
    } else {
        float ta = t0;
        float fa = gridAt(_grid, p);
        float integral = 0.0f;
        float dT = sampler.next1D()*_stepSize;
        do {
            float tb = min(ta + dT, t1);
            float fb = gridAt(_grid, p + w*tb);
            integral += (fa + fb)*0.5f*(tb - ta);
            ta = tb;
            fa = fb;
            dT = _stepSize;
        } while (ta < t1);
        return integral;
    }
}

Vec2f VdbGrid::inverseOpticalDepth(PathSampleGenerator &sampler, Vec3f p, Vec3f w, float t0, float t1,
        float sigmaT, float xi) const
{
    if (_sampleMethod == SampleMethod::ExactNearest) {
        VdbRaymarcher<openvdb::FloatGrid::TreeType, 3> dda;
        auto accessor = _grid->getConstAccessor();

        float opticalDepth = 0.0f;
        Vec2f result(t1, 0.0f);
        dda.march(DdaRay(p + 0.5f, w), t0, t1, accessor, [&](openvdb::Coord voxel, float ta, float tb) {
            float v = accessor.getValue(voxel);
            float delta = v*sigmaT*(tb - ta);
            if (opticalDepth + delta >= xi) {
                result = Vec2f(ta + (tb - ta)*(xi - opticalDepth)/delta, v);
                return true;
            }
            opticalDepth += delta;
            return false;
        });
        return result;
    } else {
        float ta = t0;
        float fa = gridAt(_grid, p)*sigmaT;
        float integral = 0.0f;
        float dT = sampler.next1D()*_stepSize;
        do {
            float tb = min(ta + dT, t1);
            float fb = gridAt(_grid, p + w*tb)*sigmaT;
            float delta = (fa + fb)*0.5f*(tb - ta);
            if (integral + delta >= xi) {
                float a = (fb - fa);
                float b = fa;
                float c = (integral - xi)/(tb - ta);
                float mantissa = max(b*b - 2.0f*a*c, 0.0f);
                float x1 = (-b + std::sqrt(mantissa))/a;
                return Vec2f(ta + (tb - ta)*x1, fa + (fb - fa)*x1);
            }
            integral += delta;
            ta = tb;
            fa = fb;
            dT = _stepSize;
        } while (ta < t1);
        return Vec2f(t1, fa);
    }
}

}

#endif
