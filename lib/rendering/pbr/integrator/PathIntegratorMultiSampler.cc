// Copyright 2023 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

/// @file PathIntegratorMultiSampler.cc

// Integration implementation based on the BsdfSampler
// 
// When making changes to this file, you'll likely also need
// to update the vector implementation and the one-sample
// integrator (both vector and scalar).
//   PathIntegratorMultiSampler.ispc
//   PathIntegratorOneSampler.cc
//   PathIntegratorOneSampler.ispc

#include "PathIntegrator.h"
#include "PathIntegratorUtil.h"

#include <moonray/rendering/pbr/core/Aov.h>
#include <moonray/rendering/pbr/core/Constants.h>
#include <moonray/rendering/pbr/core/DebugRay.h>
#include <moonray/rendering/pbr/core/RayState.h>
#include "VolumeTransmittance.h"

// using namespace scene_rdl2::math; // can't use this as it breaks openvdb in clang.

namespace moonray {
namespace pbr {

finline void
PathIntegrator::addDirectVisibleBsdfLobeSampleContribution(pbr::TLState *pbrTls,
        const Subpixel &sp, int cameraId, const PathVertex &pv,
        const BsdfSampler &bSampler, int lobeIndex, bool doIndirect, const BsdfSample &bsmp,
        const mcrt_common::RayDifferential &parentRay, float rayEpsilon, float shadowRayEpsilon,
        scene_rdl2::math::Color &radiance, unsigned& sequenceID, float *aovs,
        const shading::Intersection &isect) const
{
    MNRY_ASSERT(pbrTls->isIntegratorAccumulatorRunning());
    // This matches the behavior in the vectorized codepath.
    if (isBlack(bsmp.tDirect)) {
        return;
    }

    const Light *light = bsmp.lp.light;
    const shading::BsdfLobe &lobe = *bSampler.getLobe(lobeIndex);

    // Create a ray using the origin of the parent ray.
    // Note the origin is actually the point on the surface that
    // the parent ray hit, not the original origin of the parent ray.
    // It has been moved there when the intersection was processed.
    // The direction vector to the light is bsmp[s].wi, so use that
    // for the direction and the differential directions.
    // bsmp[s].distance is the distance from the surface point to
    // the light, so we use that for the ray length minus an
    // epsilon factor.
    const scene_rdl2::math::Vec3f& P = parentRay.getOrigin();
    float tfar = bsmp.distance * sHitEpsilonEnd;
    float time = parentRay.getTime();
    int rayDepth = parentRay.getDepth() + 1;
    float presence = 0.0f;
    scene_rdl2::math::Color contrib(0.0f);

    mcrt_common::RayDifferential shadowRay(P, bsmp.wi,
        parentRay.getOriginX(), bsmp.wi,
        parentRay.getOriginY(), bsmp.wi,
        rayEpsilon, tfar, time, rayDepth);

    // Ray termination lights are used in an attempt to cheaply fill in the zeros which result from
    // terminating ray paths too early, which is done by forcing no occlusion.
    // A path can only be terminated when doIndirect is false, but don't apply a ray termination light
    // unless we have a (non-hair) transmission lobe.
    bool isOccluded;
    if (!doIndirect && light->getIsRayTerminator()) {
        if (!lobe.matchesFlags(shading::BsdfLobe::ALL_TRANSMISSION) || lobe.getIsHair()) {
            return;
        } else {
            isOccluded = false;
        }
    } else {
        int32_t assignmentId = isect.getLayerAssignmentId();
        isOccluded = isRayOccluded(pbrTls, light, shadowRay, rayEpsilon, shadowRayEpsilon, presence, assignmentId);
    }
    if (!isOccluded) {
        // We can't reuse shadowRay because it can be modified in occlusion query
        mcrt_common::Ray trRay(P, bsmp.wi, scene_rdl2::math::max(rayEpsilon, shadowRayEpsilon), tfar, time, rayDepth);
        scene_rdl2::math::Color tr = transmittance(pbrTls, trRay, sp.mPixel, sp.mSubpixelIndex, sequenceID, light);
        contrib = tr * bsmp.tDirect * (1.0f - presence);
        radiance += contrib;
    }

    // LPE
    if (aovs) {
        EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);
        const FrameState &fs = *pbrTls->mFs;
        const AovSchema &aovSchema = *fs.mAovSchema;
        const LightAovs &lightAovs = *fs.mLightAovs;
        // transition
        int lpeStateId = pv.lpeStateId;
        lpeStateId = lightAovs.scatterEventTransition(pbrTls,
            lpeStateId, bSampler.getBsdf(), lobe);
        lpeStateId = lightAovs.lightEventTransition(pbrTls, lpeStateId, light);
        // accumulate matching aovs
        if (fs.mAovSchema->hasLpePrefixFlags(AovSchema::sLpePrefixUnoccluded)) {
            aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs, bsmp.tDirect, 
                              &contrib, AovSchema::sLpePrefixUnoccluded, lpeStateId, aovs);
        } else {
            aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs, contrib, 
                              nullptr, AovSchema::sLpePrefixNone, lpeStateId, aovs);
        }
    }

    // TODO: we don't store light intersection normal when a bsdf sample
    // ends up hitting a light. We don't store Li either.
    // If the intersection distance is closer than the distant light, then
    // assume the hit wasn't due to a distant or env light.
    if (DebugRayRecorder::isRecordingEnabled()) {
        if (bsmp.distance < sDistantLightDistance) {
            mcrt_common::Ray debugRay(parentRay.getOrigin(), bsmp.wi, 0.0f);
            RAYDB_EXTEND_RAY_NO_HIT(pbrTls, debugRay, bsmp.distance);
            RAYDB_SET_CONTRIBUTION(pbrTls, scene_rdl2::math::sWhite);
            RAYDB_ADD_TAGS(pbrTls, TAG_AREALIGHT);
        } else {
            mcrt_common::Ray debugRay(parentRay.getOrigin(), bsmp.wi, 0.0f);
            RAYDB_EXTEND_RAY_NO_HIT(pbrTls, debugRay, 40.0f);
            RAYDB_SET_CONTRIBUTION(pbrTls, scene_rdl2::math::sWhite);
            RAYDB_ADD_TAGS(pbrTls, TAG_ENVLIGHT);
        }
    }
}


void
PathIntegrator::addDirectVisibleBsdfSampleContributions(pbr::TLState *pbrTls,
        const Subpixel &sp, int cameraId, const PathVertex &pv,
        const BsdfSampler &bSampler, bool doIndirect, const BsdfSample *bsmp,
        const mcrt_common::RayDifferential &parentRay, float rayEpsilon, float shadowRayEpsilon,
        scene_rdl2::math::Color &radiance, unsigned& sequenceID, float *aovs,
        const shading::Intersection &isect) const
{
    // Trace bsdf sample shadow rays
    int s = 0;
    const int lobeCount = bSampler.getLobeCount();
    for (int lobeIndex = 0; lobeIndex < lobeCount; ++lobeIndex) {
        const int lobeSampleCount = bSampler.getLobeSampleCount(lobeIndex);
        for (int i = 0; i < lobeSampleCount; ++i, ++s) {
            if (bsmp[s].isValid() && bsmp[s].didHitLight()) {
                addDirectVisibleBsdfLobeSampleContribution(pbrTls, sp, cameraId, pv, bSampler,
                    lobeIndex, doIndirect, bsmp[s], parentRay, rayEpsilon, shadowRayEpsilon,
                    radiance, sequenceID, aovs, isect);
            }
        }
    }
}

void
PathIntegrator::addDirectVisibleLightSampleContributions(pbr::TLState *pbrTls,
        Subpixel const& sp, int cameraId, const PathVertex &pv,
        const LightSetSampler &lSampler, const LightSample *lsmp,
        const mcrt_common::RayDifferential &parentRay, float rayEpsilon, float shadowRayEpsilon,
        scene_rdl2::math::Color &radiance, unsigned& sequenceID, float *aovs,
        const shading::Intersection &isect) const
{
    MNRY_ASSERT(pbrTls->isIntegratorAccumulatorRunning());
    // Trace light sample shadow rays
    int s = 0;
    const int lightCount = lSampler.getLightCount();
    const int lightSampleCount = lSampler.getLightSampleCount();
    for (int lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
        const Light *light = lSampler.getLight(lightIndex);

        // Ray termination lights are used in an attempt to cheaply fill in the zeros which result from
        // terminating ray paths too early. We exclude them from light samples because these samples represent
        // natural ends to light paths.
        if (light->getIsRayTerminator()) {
            s += lightSampleCount;
            continue;
        }

        for (int i = 0; i < lightSampleCount; ++i, ++s) {
            if (lsmp[s].isInvalid()) {
                continue;
            }
            scene_rdl2::math::Color lightT = lsmp[s].t;
            // This matches the behavior in the vectorized codepath.
            if (isBlack(lightT)) {
                continue;
            }
            const scene_rdl2::math::Vec3f &P = parentRay.getOrigin();
            float tfar = lsmp[s].distance * sHitEpsilonEnd;
            float time = parentRay.getTime();
            int rayDepth = parentRay.getDepth() + 1;
            mcrt_common::RayDifferential shadowRay(P, lsmp[s].wi,
                parentRay.getOriginX(), lsmp[s].wi,
                parentRay.getOriginY(), lsmp[s].wi,
                rayEpsilon, tfar, time, rayDepth);
            float presence = 0.0f;
            scene_rdl2::math::Color tr;

            const FrameState &fs = *pbrTls->mFs;
            const bool hasUnoccludedFlag = fs.mAovSchema->hasLpePrefixFlags(AovSchema::sLpePrefixUnoccluded);
            int32_t assignmentId = isect.getLayerAssignmentId();
            if (isRayOccluded(pbrTls, light, shadowRay, rayEpsilon, shadowRayEpsilon, presence, assignmentId)) {
                // Calculate clear radius falloff
                // only do extra calculations if clear radius falloff enabled
                if (light->getClearRadiusFalloffDistance() != 0.f && 
                    tfar < light->getClearRadius() + light->getClearRadiusFalloffDistance()) {
                    // compute unoccluded pixel value
                    lightT *= (1.0f - presence);
                    mcrt_common::Ray trRay(P, lsmp[s].wi, scene_rdl2::math::max(rayEpsilon, shadowRayEpsilon), tfar, time, rayDepth);
                    tr = transmittance(pbrTls, trRay, sp.mPixel, sp.mSubpixelIndex, sequenceID, light);

                    radiance += calculateShadowFalloff(light, tfar, tr * lightT);  
                }

                // Visibility LPE
                // A ray is occluded, we must record that for the shadow aov.
                if (aovs) {
                    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);
                    const LightAovs &lightAovs = *fs.mLightAovs;

                    // If there is no visibility AOV and if we don't have unoccluded flags, then we don't need to bother
                    // with accumulating these values here.
                    if (lightAovs.hasVisibilityEntries() || hasUnoccludedFlag) {

                        const AovSchema &aovSchema = *fs.mAovSchema;
                        const Light *light = lSampler.getLight(lightIndex);
                        bool addVisibility = true;
                        for (unsigned l = 0; l < shading::Bsdf::maxLobes; ++l) {
                            if (!lsmp[s].lp.lobe[l]) continue;

                            const shading::BsdfLobe &lobe = *lsmp[s].lp.lobe[l];
                            const scene_rdl2::math::Color &unoccludedLobeVal = lsmp[s].lp.t[l];
                            int lpeStateId = pv.lpeStateId;
                            // transition
                            lpeStateId = lightAovs.scatterEventTransition(pbrTls,
                                lpeStateId, lSampler.getBsdf(), lobe);
                            lpeStateId = lightAovs.lightEventTransition(pbrTls,
                                lpeStateId, light);

                            // visibility aov
                            if (addVisibility) {
                                if (aovAccumVisibilityAovs(pbrTls, aovSchema, cameraId, lightAovs,
                                    scene_rdl2::math::Vec2f(0.0f, 1.0f), lpeStateId, aovs)) {
                                    // add visibility aov at most once per shadow ray
                                    addVisibility = false;
                                }
                            }

                            // unoccluded prefix LPEs
                            if (hasUnoccludedFlag) {
                                // If it's occluded but we have the unoccluded flag set, only contribute this to any 
                                // pre-occlusion aovs.
                                aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs, unoccludedLobeVal, nullptr,
                                                  AovSchema::sLpePrefixUnoccluded, lpeStateId, aovs);
                            }
                        }
                    }
                }
            } else {
                // Take into account presence and transmittance
                lightT *= (1.0f - presence);
                mcrt_common::Ray trRay(P, lsmp[s].wi, scene_rdl2::math::max(rayEpsilon, shadowRayEpsilon), tfar, time, rayDepth);
                tr = transmittance(pbrTls, trRay, sp.mPixel, sp.mSubpixelIndex, sequenceID, light);
                radiance += tr * lightT;

                // LPE
                if (aovs) {
                    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);
                    const AovSchema &aovSchema = *fs.mAovSchema;
                    const LightAovs &lightAovs = *fs.mLightAovs;
                    const Light *light = lSampler.getLight(lightIndex);
                    bool addVisibility = true;
                    for (unsigned l = 0; l < shading::Bsdf::maxLobes; ++l) {
                        if (!lsmp[s].lp.lobe[l]) continue;
                        const shading::BsdfLobe &lobe = *lsmp[s].lp.lobe[l];
                        const scene_rdl2::math::Color &unoccludedLobeVal = lsmp[s].lp.t[l];
                        const scene_rdl2::math::Color &lobeVal = tr * lsmp[s].lp.t[l] * (1.0f - presence);
                        const shading::Bsdf &bsdf = lSampler.getBsdf();
                        // transition
                        int lpeStateId = pv.lpeStateId;
                        lpeStateId = lightAovs.scatterEventTransition(pbrTls,
                            lpeStateId, bsdf, lobe);
                        lpeStateId = lightAovs.lightEventTransition(pbrTls,
                            lpeStateId, light);

                        // Accumulate aovs depending on whether or not the unoccluded flag is set.
                        if (hasUnoccludedFlag) {
                            // If the unoccluded flag is set we have to add occluded and unoccluded 
                            // (without presence and volume transmittance) separately.
                            aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs, unoccludedLobeVal, &lobeVal, 
                                              AovSchema::sLpePrefixUnoccluded, lpeStateId, aovs);
                        } else {
                            // Otherwise, just add the contribution to all non-pre-occlusion aovs.
                            aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs, lobeVal, nullptr, 
                                              AovSchema::sLpePrefixNone, lpeStateId, aovs);
                        }

                        // visibility aov
                        if (addVisibility) {
                            if (aovAccumVisibilityAovs(pbrTls, aovSchema, cameraId, lightAovs,
                                scene_rdl2::math::Vec2f(reduceTransparency(tr) * (1 - presence), 1.0f),
                                lpeStateId, aovs)) {
                                // add visibility aov at most once per shadow ray
                                addVisibility = false;
                            }
                        }
                    }
                }

                // TODO: we don't store light sample normal
                // If the intersection distance is closer than the distant light, then
                // assume the hit wasn't due to a distant or env light.
                if (DebugRayRecorder::isRecordingEnabled()) {
                    if (lsmp[s].distance < sDistantLightDistance) {
                        mcrt_common::Ray debugRay(P, lsmp[s].wi, 0.0f);
                        RAYDB_EXTEND_RAY_NO_HIT(pbrTls, debugRay, lsmp[s].distance);
                        RAYDB_SET_CONTRIBUTION(pbrTls, lsmp[s].Li);
                        RAYDB_ADD_TAGS(pbrTls, TAG_AREALIGHT);
                    } else {
                        mcrt_common::Ray debugRay(P, lsmp[s].wi, 0.0f);
                        RAYDB_EXTEND_RAY_NO_HIT(pbrTls, debugRay, 40.0f);
                        RAYDB_SET_CONTRIBUTION(pbrTls, lsmp[s].Li);
                        RAYDB_ADD_TAGS(pbrTls, TAG_ENVLIGHT);
                    }
                }
            }
        }
    }
}


void
PathIntegrator::addIndirectOrDirectVisibleContributions(
    pbr::TLState *pbrTls,
    const Subpixel &sp, int cameraId, const PathVertex &parentPv, const BsdfSampler &bSampler,
    const BsdfSample *bsmp, const mcrt_common::RayDifferential &parentRay,
    float rayEpsilon, float shadowRayEpsilon,
    const shading::Intersection &isect, shading::BsdfLobe::Type indirectFlags,
    const scene_rdl2::rdl2::Material* newPriorityList[4], int newPriorityListCount[4],
    scene_rdl2::math::Color &radiance, unsigned& sequenceID,
    float *aovs) const
{
    MNRY_ASSERT(pbrTls->isIntegratorAccumulatorRunning());

    scene_rdl2::math::Vec3f wo = -parentRay.getDirection();

    // Trace bsdf sample continuation rays. We accumulate either direct or
    // indirect lighting contributions accordingly
    int s = 0;
    const int lobeCount = bSampler.getLobeCount();
    for (int lobeIndex = 0; lobeIndex < lobeCount; ++lobeIndex) {
        const shading::BsdfLobe *lobe = bSampler.getLobe(lobeIndex);

        // hair lobe type is a special case that often requires more bounces.
        const bool hairLobe = lobe->getIsHair();
        const bool doIndirect = lobe->matchesFlags(indirectFlags)
                                || (hairLobe && (parentPv.hairDepth < mMaxHairDepth));
        const bool diffuseLobe = lobe->matchesFlags(shading::BsdfLobe::ALL_DIFFUSE);
        const bool glossyLobe = lobe->matchesFlags(shading::BsdfLobe::ALL_GLOSSY);
        const bool mirrorLobe = lobe->matchesFlags(shading::BsdfLobe::ALL_MIRROR);
        const bool transmissionLobe = lobe->matchesFlags(shading::BsdfLobe::ALL_TRANSMISSION);

        const scene_rdl2::math::Vec2f minRoughness = (doIndirect  ?  computeMinRoughness(*lobe,
                mRoughnessClampingFactor, parentPv.minRoughness)  :  scene_rdl2::math::Vec2f(0.0f));

        const int lobeSampleCount = bSampler.getLobeSampleCount(lobeIndex);
        for (int i = 0; i < lobeSampleCount; ++i, ++s) {
            if (bsmp[s].isInvalid()) {
                continue;
            }

            // If this sample hit a light, compute direct lighting contribution only
            if (bsmp[s].didHitLight()) {
                addDirectVisibleBsdfLobeSampleContribution(pbrTls, sp, cameraId, parentPv, bSampler,
                    lobeIndex, doIndirect, bsmp[s], parentRay, rayEpsilon, shadowRayEpsilon,
                    radiance, sequenceID, aovs, isect);
            }

            if (!doIndirect) {
                continue;
            }

            // We have some self-intersections when rays leave at grazing
            // angle, so we adjust the rayEpsilon accordingly.
            // We only trace up to the bsmp[s].distance. It is set to the distance
            // to the intersected light in this direction, if any.
            const float denom = scene_rdl2::math::abs(dot(isect.getNg(), bsmp[s].wi));
            // isect.getNg() itself or the dot product above can be zero.
            const float start = scene_rdl2::math::isZero(denom) ? rayEpsilon : rayEpsilon / denom;
            float end = (bsmp[s].distance == scene_rdl2::math::sMaxValue  ?  scene_rdl2::math::sMaxValue  :
                         bsmp[s].distance * sHitEpsilonEnd);
            if (end <= start) {
                continue;
            }

            // Check transparency threshold
            float newAccumOpacity;
            if (mirrorLobe && lobe->matchesFlags(shading::BsdfLobe::ALL_TRANSMISSION)) {
                float lobeTransparency = reduceTransparency(bsmp[s].f);
                newAccumOpacity = parentPv.accumOpacity + (1 - lobeTransparency) * (1 - parentPv.accumOpacity);
                if (newAccumOpacity > mTransparencyThreshold) {
                    continue;
                }
            } else {
                newAccumOpacity = parentPv.accumOpacity;
            }

            // Prepare a path vertex
            PathVertex pv;
            pv.pathThroughput = bsmp[s].tIndirect;
            pv.pathPixelWeight = 0.0f;
            // Use previous path pixel weight for aovPathPixelWeight as there's existing logic
            // in vector mode that sometimes assumes that pv.pathPixelWeight = 0.  Thus, we must seperately
            // keep track of the pathPixelWeight for aovs.  See comment in PathIntegratorMultiSampler.ispc::
            // addIndirectOrDirectVisibleContributionsBundled().
            pv.aovPathPixelWeight = parentPv.pathPixelWeight;
            pv.pathDistance = parentPv.pathDistance + parentRay.getEnd();
            pv.minRoughness = minRoughness;
            pv.diffuseDepth = parentPv.diffuseDepth + (diffuseLobe ? 1 : 0);
            pv.glossyDepth = parentPv.glossyDepth + (glossyLobe ? 1 : 0);
            pv.mirrorDepth = parentPv.mirrorDepth + (mirrorLobe ? 1 : 0);
            pv.nonMirrorDepth = parentPv.nonMirrorDepth + (mirrorLobe ? 0 : 1);
            pv.hairDepth = parentPv.hairDepth + (hairLobe ? 1 : 0);
            pv.volumeDepth = parentPv.volumeDepth + 1;
            pv.presenceDepth = parentPv.presenceDepth;
            pv.totalPresence = parentPv.totalPresence;
            pv.subsurfaceDepth = parentPv.subsurfaceDepth;
            pv.accumOpacity = newAccumOpacity;
            pv.lobeType = lobe->getType();

            // LPE
            if (aovs) {
                const FrameState &fs =*pbrTls->mFs;
                const LightAovs &lightAovs = *fs.mLightAovs;
                const shading::Bsdf &bsdf = bSampler.getBsdf();

                // transition
                pv.lpeStateId = lightAovs.scatterEventTransition(pbrTls, parentPv.lpeStateId, bsdf, *lobe);

                // Accumulate post scatter extra aovs
                aovAccumPostScatterExtraAovs(pbrTls, fs, pv, cameraId, bsdf, aovs);
            }
            // Prepare a mcrt_common::RayDifferential
            mcrt_common::RayDifferential ray(parentRay, start, end);

            if (transmissionLobe) {
                // copy in the new priority list into the ray
                setPriorityList(ray, newPriorityList, newPriorityListCount);
            } else {
                // even if it isn't a transmission lobe, we need to copy the parent's material priority list
                const scene_rdl2::rdl2::Material* prevPriorityList[4];
                int prevPriorityListCount[4]; 
                getPriorityList(parentRay, prevPriorityList, prevPriorityListCount);
                setPriorityList(ray, prevPriorityList, prevPriorityListCount);
            }

            // Scatter and scale our next ray differential
            // We scatter the ray based on the sampled lobe
            scatterAndScale(isect, *lobe, wo, bsmp[s].wi,
                ((lobe->getDifferentialFlags() & shading::BsdfLobe::IGNORES_INCOMING_DIFFERENTIALS) ?
                 sp.mPrimaryRayDiffScale : 1.0f),
                bsmp[s].sample[0], bsmp[s].sample[1], ray);

            CHECK_CANCELLATION(pbrTls, return);

            // Recurse
            scene_rdl2::math::Color radianceIndirect;
            float transparencyIndirect;
            // the volume attenuation along this ray to the first hit (or infinity)
            VolumeTransmittance vtIndirect;
            ++sequenceID;
            bool hitVolume;

            IndirectRadianceType indirectRadianceType = computeRadianceRecurse(
                    pbrTls, ray, sp, cameraId,
                    pv, lobe, radianceIndirect, transparencyIndirect,
                    vtIndirect, sequenceID, aovs, nullptr, nullptr, nullptr, false, hitVolume);

            if (indirectRadianceType != NONE) {
                // Accumulate indirect lighting contribution
                // In the bundled version, this contribution gets accounted for by
                // queueing a BundledRadiance for the direct radiance and/or emission,
                // and spawning one or more new rays for the indirect radiance.
                radiance += radianceIndirect;

                // path guiding is trained on indirect radiance contributions.
                // it turns out that including any other radiance contributions in the
                // training causes the path guide to overwhelmingly favor direct lighting
                // directions.  this is the exact opposite of what we want.  we are trying
                // to build a distribution that favors important indirect lighting.
                mPathGuide.recordRadiance(parentRay.getOrigin(), bsmp[s].wi, radianceIndirect);
            }
            if (!bsmp[s].didHitLight()) {
                const FrameState &fs = *pbrTls->mFs;
                const AovSchema &aovSchema = *fs.mAovSchema;
                const LightAovs &lightAovs = *fs.mLightAovs;
                if (fs.mAovSchema->hasLpePrefixFlags(AovSchema::sLpePrefixUnoccluded)) {
                    // transition
                    int lpeStateId = pv.lpeStateId;
                    lpeStateId = lightAovs.lightEventTransition(pbrTls,
                        lpeStateId, bsmp[s].lp.light);
                    // accumulate matching aovs.
                    aovAccumLightAovs(pbrTls, aovSchema, cameraId, lightAovs,
                            bsmp[s].tDirect, nullptr, AovSchema::sLpePrefixNone, lpeStateId, aovs);
                }
            }
        }
    }
}

scene_rdl2::math::Color
PathIntegrator::computeRadianceBsdfMultiSampler(pbr::TLState *pbrTls,
    const Subpixel &sp, int cameraId, const PathVertex &pv, const mcrt_common::RayDifferential &ray,
    const shading::Intersection &isect, const shading::Bsdf &bsdf, const shading::BsdfSlice &slice,
    bool doIndirect, const shading::BsdfLobe::Type indirectFlags, const scene_rdl2::rdl2::Material *newPriorityList[4],
    int newPriorityListCount[4], const LightSet &activeLightSet, const scene_rdl2::math::Vec3f *cullingNormal,
    float rayEpsilon, float shadowRayEpsilon,
    const scene_rdl2::math::Color &ssAov, unsigned &sequenceID, float *aovs) const
{
    // TODO:
    // A couple things seem out of place with the parameters to function.
    //
    // First, why do we require both a doIndirect boolean and an indirectFlags?
    // Ideally we could determine doIndirect from the indirecFlags.  But there is
    // some complication with hair depth that prevents this.
    //
    // Second, the ssAov parameter is unfortunate.  It is just passed along to the
    // material aov packing function.  I think it would be best to pack the material
    // aovs before calling this function.  But that would require the elimination of
    // the "albedo" material aov - which relies on the bsdf samples.  That might actually
    // be a good thing.

    // In case it isn't clear, the outputs of this function are the returned radiance
    // and accumulated aovs.  The sequenceID is both and input
    // and an output.
    scene_rdl2::math::Color radiance = scene_rdl2::math::sBlack;

    // We use a BsdfSampler object to keep track of sampling strategies and
    // sample budget per lobe.
    // We only want to split on the first scattering event seen from the
    // camera (either directly, either through one or many mirror bounces)
    const int maxSamplesPerLobe = (pv.nonMirrorDepth == 0  ?  mBsdfSamples  :
            scene_rdl2::math::min(mBsdfSamples, 1));

    scene_rdl2::alloc::Arena *arena = pbrTls->mArena;

    // TODO: I feel that the bsdf is conceptually const at this point
    // I don't know why or for what reason the bsdf sampler needs a non-const bsdf
    shading::Bsdf &constCastBsdf = const_cast<shading::Bsdf &>(bsdf);

    BsdfSampler bSampler(arena, constCastBsdf, slice, maxSamplesPerLobe, doIndirect, mPathGuide);

    const int bsdfSampleCount = bSampler.getSampleCount();
    BsdfSample *bsmp = arena->allocArray<BsdfSample>(bsdfSampleCount);

    // We use a LightSetSampler object to keep track of sampling strategies and
    // sample budget per light.
    // We use the same splitting strategy as for lobes above.
    const int maxSamplesPerLight = (pv.nonMirrorDepth == 0  ?  mLightSamples  :
            scene_rdl2::math::min(mLightSamples, 1));
    LightSetSampler lSampler(arena, activeLightSet, bsdf, isect.getP(), maxSamplesPerLight);

    const int lightSetSampleCount = lSampler.getSampleCount();
    LightSample *lsmp = arena->allocArray<LightSample>(lightSetSampleCount);

    // Draw Bsdf and LightSet samples and compute tentative contributions.
    drawBsdfSamples(pbrTls, bSampler, lSampler, sp, pv, isect.getP(), cullingNormal,
                    ray.getTime(), sequenceID, bsmp, mSampleClampingDepth,
                    sp.mSampleClampingValue, indirectFlags, ray.getDirFootprint());

    // now that we have drawn our bsdf samples, we can fully
    // pack our material aovs
    if (aovs) {
        const FrameState &fs = *pbrTls->mFs;
        const AovSchema &aovSchema = *fs.mAovSchema;
        const Scene *scene = MNRY_VERIFY(pbrTls->mFs->mScene);
        aovSetMaterialAovs(pbrTls, aovSchema, cameraId, *fs.mLightAovs, *fs.mMaterialAovs,
                           isect, ray, *scene, bsdf, ssAov,
                           &bSampler, bsmp, pv.aovPathPixelWeight, pv.lpeStateId, aovs);
    }

    drawLightSetSamples(pbrTls, lSampler, bSampler, sp, pv, isect.getP(), cullingNormal, ray.getTime(),
                        sequenceID, lsmp, mSampleClampingDepth, sp.mSampleClampingValue, ray.getDirFootprint());

    CHECK_CANCELLATION(pbrTls, return scene_rdl2::math::sBlack );

    //---------------------------------------------------------------------
    // Apply Russian Roulette (RR). Note we only do RR past a non-mirror
    // bounce, to avoid breaking the nice stratification of samples on the
    // first non-mirror hit.
    // TODO: We should find a way to use efficiency optimized RR. For this we
    // need an estimate of the final pixel color so our threshold can be
    // computed accordingly.
    if (pv.nonMirrorDepth > 0  &&  mRussianRouletteThreshold > 0.0f) {
        applyRussianRoulette(bSampler, bsmp, sp, pv, sequenceID,
                mRussianRouletteThreshold, mInvRussianRouletteThreshold);
        applyRussianRoulette(lSampler, lsmp, sp, pv, sequenceID,
                mRussianRouletteThreshold, mInvRussianRouletteThreshold);
    }

    CHECK_CANCELLATION(pbrTls, return scene_rdl2::math::sBlack );

    //---------------------------------------------------------------------
    // Let's trace some rays to resolve visibility / shadowing and sum up
    // contributions. We trace one ray per valid sample and affect all
    // contributions for that sample accordingly.

    if (doIndirect) {

        // Note: This will recurse
        addIndirectOrDirectVisibleContributions(pbrTls, sp, cameraId, pv, bSampler, bsmp,
                ray, rayEpsilon, shadowRayEpsilon, isect, indirectFlags, newPriorityList, newPriorityListCount,
                radiance, sequenceID, aovs);
        checkForNan(radiance, "Direct or indirect contributions", sp, pv, ray,
                isect);
    } else {
        // TODO: Incorrect transparency if there is no indirect
        addDirectVisibleBsdfSampleContributions(pbrTls, sp, cameraId, pv, bSampler, false, bsmp, ray,
            rayEpsilon, shadowRayEpsilon, radiance, sequenceID, aovs, isect);
        checkForNan(radiance, "Direct contributions", sp, pv, ray, isect);
    }
    addDirectVisibleLightSampleContributions(pbrTls, sp, cameraId, pv, lSampler, lsmp, ray,
            rayEpsilon, shadowRayEpsilon, radiance, sequenceID, aovs, isect);
    checkForNan(radiance, "Direct contributions", sp, pv, ray, isect);

    return radiance;
}

} // namespace pbr
} // namespace moonray

