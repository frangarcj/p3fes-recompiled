#pragma once

#include "p3fes_rw_matrix_math.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace p3fes_skin_palette_math
{
    using Matrix = p3fes_rw_matrix_math::Matrix;

    enum class HierarchyMode
    {
        FrameNodes,
        LocalMatrices,
        WorldMatrices,
    };

    // Match the branch order in 0x46B510: flags&2 wins, then 0x4000, then
    // the unflagged +8 field is a world-space matrix array.
    inline HierarchyMode hierarchyMode(uint32_t flags)
    {
        if ((flags & 0x2u) != 0u)
            return HierarchyMode::FrameNodes;
        if ((flags & 0x4000u) != 0u)
            return HierarchyMode::LocalMatrices;
        return HierarchyMode::WorldMatrices;
    }

    inline Matrix identity()
    {
        Matrix result{};
        result[0] = result[5] = result[10] = result[15] = 1.0f;
        return result;
    }

    // RwMatrixInvert's fast path for an orthonormal matrix (matrix type bits
    // equal 3): transpose its 3x3 basis and transform the translation by
    // -R^T. The caller handles other flag combinations with the guest helper.
    inline bool invertOrthonormal(const Matrix &source, Matrix &out)
    {
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 3; ++row)
                if (!p3fes_rw_matrix_math::ordinary(source[column * 4 + row]))
                    return false;

        Matrix result{};
        result[0] = source[0];
        result[1] = source[4];
        result[2] = source[8];
        result[4] = source[1];
        result[5] = source[5];
        result[6] = source[9];
        result[8] = source[2];
        result[9] = source[6];
        result[10] = source[10];
        result[12] = -(result[0] * source[12] + result[4] * source[13] + result[8] * source[14]);
        result[13] = -(result[1] * source[12] + result[5] * source[13] + result[9] * source[14]);
        result[14] = -(result[2] * source[12] + result[6] * source[13] + result[10] * source[14]);
        result[15] = 1.0f;
        out = result;
        return true;
    }

    // RwMatrixInvert's generic callee 0x4C2860. Keep its cofactor and
    // determinant expression order; for det==0 the guest uses invdet=1 and
    // returns the unscaled cofactor matrix.
    inline bool invertGeneral(const Matrix &source, Matrix &out)
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 3; ++row)
                if (!p3fes_rw_matrix_math::ordinary(source[column * 4 + row]))
                    return false;

        Matrix result{};
        result[0] = source[5] * source[10] - source[6] * source[9];
        result[1] = -(source[1] * source[10] - source[2] * source[9]);
        result[2] = source[1] * source[6] - source[2] * source[5];

        // The guest accumulates result[1]*source[4], result[0]*source[0],
        // then result[2]*source[8] in that order using MULA/MADDA/MADD.
        const float determinant0 = result[1] * source[4];
        const float determinant1 = determinant0 + result[0] * source[0];
        const float determinant = determinant1 + result[2] * source[8];
        uint32_t determinantBits = 0u;
        std::memcpy(&determinantBits, &determinant, sizeof(determinantBits));
        // The generated branch tests raw bits, so -0.0 falls through to FPU
        // divide and must retain the guest path's exceptional semantics.
        if (determinantBits == 0x80000000u ||
            (determinantBits & 0x7F800000u) == 0x7F800000u ||
            !p3fes_rw_matrix_math::ordinary(determinant))
            return false;
        const float inverseDeterminant = determinantBits == 0u ? 1.0f : 1.0f / determinant;

        result[0] *= inverseDeterminant;
        result[1] *= inverseDeterminant;
        result[2] *= inverseDeterminant;
        result[4] = inverseDeterminant * (-(source[4] * source[10] - source[6] * source[8]));
        result[5] = inverseDeterminant * (source[10] * source[0] - source[8] * source[2]);
        result[6] = inverseDeterminant * (-(source[0] * source[6] - source[2] * source[4]));
        result[8] = inverseDeterminant * (source[4] * source[9] - source[5] * source[8]);
        result[9] = inverseDeterminant * (-(source[0] * source[9] - source[8] * source[1]));
        result[10] = inverseDeterminant * (source[0] * source[5] - source[1] * source[4]);
        const float posX0 = source[13] * result[4];
        const float posX1 = posX0 + source[12] * result[0];
        result[12] = -(posX1 + source[14] * result[8]);
        const float posY0 = source[13] * result[5];
        const float posY1 = posY0 + source[12] * result[1];
        result[13] = -(posY1 + source[14] * result[9]);
        const float posZ0 = source[13] * result[6];
        const float posZ1 = posZ0 + source[12] * result[2];
        result[14] = -(posZ1 + source[14] * result[10]);
        result[15] = 1.0f;
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 3; ++row)
                if (!p3fes_rw_matrix_math::ordinary(result[column * 4 + row]))
                    return false;
        out = result;
        return true;
    }

    // RwMatrixInvert has a special copy path when both the global matrix mode
    // and source flags carry 0x20000. Otherwise only use the exact orthonormal
    // fast path or the generic affine inverse from 0x4C2860.
    inline bool invertRwMatrix(const Matrix &source, uint32_t sourceFlags,
                               uint32_t globalFlags, Matrix &out)
    {
        if ((sourceFlags & globalFlags & 0x20000u) != 0u)
        {
            out = source;
            return true;
        }
        if ((sourceFlags & 0x3u) == 0x3u)
            return invertOrthonormal(source, out);
        return invertGeneral(source, out);
    }

    inline bool buildIdentityPalette(uint32_t count, std::vector<Matrix> &out)
    {
        if (count == 0u)
            return false;
        out.assign(count, identity());
        return true;
    }

    // HAnim LOCALSPACEMATRICES branch: hierarchy matrices are already in
    // atomic-local space and the guest computes localMatrix * inverseBind.
    inline bool buildLocalPalette(const std::vector<Matrix> &localMatrices,
                                  const std::vector<Matrix> &inverseBind,
                                  std::vector<Matrix> &out)
    {
        if (localMatrices.empty() || localMatrices.size() != inverseBind.size())
            return false;
        out.resize(localMatrices.size());
        std::fill(out.begin(), out.end(), identity());
        for (std::size_t i = 0; i < localMatrices.size(); ++i)
            if (!p3fes_rw_matrix_math::multiply(inverseBind[i], localMatrices[i], out[i]))
                return false;
        return true;
    }

    // Frame-derived branch: the ELF forms boneLTM * inverseBind, then
    // inverseAtomicLTM * that result.
    // The palette only feeds the renderer, so it does not need the guest-exact, operand-classifying
    // p3fes_rw_matrix_math::multiply (which was 3-5% of the game thread: two calls per bone of every skinned atomic).
    // Same formula and operation order, plain arithmetic (NEON where available, no FMA).
    // All pointers address 16 floats. Writes all 16 floats of out (padding lanes 0,0,0,1 like identity()).
    // Returns false when any xyz result is not finite: a non-finite intermediate always reaches the result,
    // since every intermediate lane feeds three result lanes through a multiply and an add.
    inline bool framePaletteEntry(const float *inverseAtomicLtm, const float *boneLtm,
                                  const float *inverseBind, float *out)
    {
        alignas(16) float boneTimesBind[16];
        p3fes_rw_matrix_math::multiplyPlainAffine(inverseBind, boneLtm, boneTimesBind);
        p3fes_rw_matrix_math::multiplyPlainAffine(boneTimesBind, inverseAtomicLtm, out);
        return p3fes_rw_matrix_math::allFiniteXyz(out);
    }

    inline bool buildFramePalette(const Matrix &inverseAtomicLtm,
                                  const std::vector<Matrix> &boneLtms,
                                  const std::vector<Matrix> &inverseBind,
                                  std::vector<Matrix> &out)
    {
        if (boneLtms.empty() || boneLtms.size() != inverseBind.size())
            return false;
        out.resize(boneLtms.size());
        bool finite = true;
        for (std::size_t i = 0; i < boneLtms.size(); ++i)
            finite &= framePaletteEntry(inverseAtomicLtm.data(), boneLtms[i].data(),
                                        inverseBind[i].data(), out[i].data());
        // Not finite somewhere: let the caller fall back, as the exact multiply did.
        return finite;
    }
} // namespace p3fes_skin_palette_math
