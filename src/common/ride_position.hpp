#pragma once
#include "ride_geometry.hpp"
#include "scope_position_program.hpp"
#include <algorithm>

namespace ss2vr {
// Admission only, not GPU precision or a replacement animation evaluator.
// Inverting the INDEPENDENT executed root camera prevents a collapsed clip
// transform from passing a loose comparison of near-zero coordinates.
inline bool rideCameraInverse(const Matrix44 &camera,std::array<double,16> &inverse) {
    inverse={};double rows[4][8]{};double norm=0;
    for(unsigned r=0;r<4;++r) {
        double sum=0;
        for(unsigned c=0;c<4;++c) {
            const double value=camera.m[4*r+c];if(!std::isfinite(value))return false;
            rows[r][c]=value;sum+=std::abs(value);
        }
        norm=std::max(norm,sum);rows[r][4+r]=1;
    }
    for(unsigned c=0;c<4;++c) {
        unsigned pivot=c;
        for(unsigned r=c+1;r<4;++r)if(std::abs(rows[r][c])>std::abs(rows[pivot][c]))pivot=r;
        if(!std::isfinite(rows[pivot][c]) || rows[pivot][c]==0)return false;
        if(pivot!=c)for(unsigned k=0;k<8;++k)std::swap(rows[pivot][k],rows[c][k]);
        const double divisor=rows[c][c];for(auto &v:rows[c])v/=divisor;
        for(unsigned r=0;r<4;++r)if(r!=c) {
            const double factor=rows[r][c];for(unsigned k=0;k<8;++k)rows[r][k]-=factor*rows[c][k];
        }
    }
    double inverseNorm=0;
    for(unsigned r=0;r<4;++r) {
        double sum=0;
        for(unsigned c=0;c<4;++c) {
            const double value=rows[r][4+c];if(!std::isfinite(value))return false;
            inverse[4*r+c]=value;sum+=std::abs(value);
        }
        inverseNorm=std::max(inverseNorm,sum);
    }
    // Conservative diagnostic policy; unsupported large/ill-conditioned views
    // decline. It must not silently relax the millimetre world-space bound.
    if(!std::isfinite(norm*inverseNorm) || norm*inverseNorm>1e6)return false;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        double product=0;
        for(unsigned k=0;k<4;++k)product+=double(camera.m[4*r+k])*inverse[4*k+c];
        if(std::abs(product-(r==c?1:0))>1e-8)return false;
    }
    return true;
}
inline bool rideHandlesPosition(const RideHandleGeometry &geometry,uint32_t paletteCount,uint32_t mainSlot,
        std::span<const uint32_t> program,std::span<const std::array<float,4>> constants,GeometryInputLayout layout,
        const Matrix34 &modelWorld,const Matrix34 &main,const Matrix34 &view,const Matrix44 &projection,
        std::array<std::array<Vec3,RideHandleMesh::MaxVertices>,2> &world) {
    world={};
    if(!geometry.copied || geometry.profile==RideHandleProfile::Unknown || !paletteCount || paletteCount>32 ||
       mainSlot>=paletteCount || !vertexPositionProgram(program) || constants.empty())return false;
    const auto affine=affineMultiply(modelWorld,main);
    Matrix34 affineInv{};
    if(!affineInverse(affine,affineInv))return false;
    Matrix44 camera{},clip{};
    const Matrix34 identity{{1,0,0,0,0,1,0,0,0,0,1,0}};
    if(!scopeCapClip(projection,view,identity,camera) || !scopeCapClip(projection,view,affine,clip))return false;
    std::array<double,16> cameraInverse{};
    if(!rideCameraInverse(camera,cameraInverse))return false;
    std::array<std::array<Vec3,RideHandleMesh::MaxVertices>,2> next{};
    for(unsigned hand=0;hand<2;++hand) {
        const auto &mesh=geometry.handles[hand];
        if(!mesh.vertices || mesh.vertices>RideHandleMesh::MaxVertices)return false;
        for(unsigned vertex=0;vertex<mesh.vertices;++vertex) {
            const auto p=mesh.positions[vertex];
            const scope_position::VehicleRigidPaletteInput input{mesh.localIndices[vertex],mesh.weights[vertex],paletteCount};
            if(input.indices[0]!=mainSlot || !input.valid())return false;
            std::array<float,4> actual{};
            if(!scope_position::position(program,constants,p,mesh.uv[vertex],true,actual,layout,nullptr,&input))return false;
            for(unsigned r=0;r<4;++r) {
                const double expected=double(clip.m[4*r])*p.x+double(clip.m[4*r+1])*p.y+
                    double(clip.m[4*r+2])*p.z+clip.m[4*r+3];
                if(!std::isfinite(actual[r]) || !std::isfinite(expected) ||
                   std::abs(actual[r]-expected)>1e-5+1e-5*std::max(std::abs(double(actual[r])),std::abs(expected)))return false;
            }
            const Vec3 expectedWorld{affine.m[0]*p.x+affine.m[1]*p.y+affine.m[2]*p.z+affine.m[3],
                affine.m[4]*p.x+affine.m[5]*p.y+affine.m[6]*p.z+affine.m[7],
                affine.m[8]*p.x+affine.m[9]*p.y+affine.m[10]*p.z+affine.m[11]};
            double recovered[4]{};
            for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)recovered[r]+=cameraInverse[4*r+k]*actual[k];
            if(!std::isfinite(recovered[3]) || std::abs(recovered[3])<1e-12)return false;
            const double expected[3]{expectedWorld.x,expectedWorld.y,expectedWorld.z};
            for(unsigned r=0;r<3;++r)if(!std::isfinite(recovered[r]) || !std::isfinite(expected[r]) ||
                std::abs(recovered[r]/recovered[3]-expected[r])>.001)return false;
            next[hand][vertex]=expectedWorld;
        }
    }
    world=next;return true;
}
// The handles' world positions from the model's world and Main palette
// transforms alone (mesh and palette input checked as above). Single-pass
// stereo rewrites the vertex constants, so the emulated clip comparison of
// rideHandlesPosition cannot agree in VR; the transforms are the same ones.
inline bool rideHandlesWorld(const RideHandleGeometry &geometry,uint32_t paletteCount,uint32_t mainSlot,
        const Matrix34 &modelWorld,const Matrix34 &main,std::array<std::array<Vec3,RideHandleMesh::MaxVertices>,2> &world) {
    world={};
    if(!geometry.copied || geometry.profile==RideHandleProfile::Unknown || !paletteCount || paletteCount>32 ||
       mainSlot>=paletteCount)return false;
    const auto affine=affineMultiply(modelWorld,main);
    std::array<std::array<Vec3,RideHandleMesh::MaxVertices>,2> next{};
    for(unsigned hand=0;hand<2;++hand) {
        const auto &mesh=geometry.handles[hand];
        if(!mesh.vertices || mesh.vertices>RideHandleMesh::MaxVertices)return false;
        for(unsigned vertex=0;vertex<mesh.vertices;++vertex) {
            const auto p=mesh.positions[vertex];
            const scope_position::VehicleRigidPaletteInput input{mesh.localIndices[vertex],mesh.weights[vertex],paletteCount};
            if(input.indices[0]!=mainSlot || !input.valid())return false;
            const Vec3 w{affine.m[0]*p.x+affine.m[1]*p.y+affine.m[2]*p.z+affine.m[3],
                affine.m[4]*p.x+affine.m[5]*p.y+affine.m[6]*p.z+affine.m[7],
                affine.m[8]*p.x+affine.m[9]*p.y+affine.m[10]*p.z+affine.m[11]};
            if(!std::isfinite(w.x+w.y+w.z))return false;
            next[hand][vertex]=w;
        }
    }
    world=next;return true;
}
} // namespace ss2vr
