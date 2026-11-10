#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_backend_state.h" // state to replay the same commands.
#include "runtime/gs/gs_texture_page_cache.h"
#include "runtime/gs/gs_upload24.h" // pixel continuation between uploads.

#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

class GSCpuBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

    // Hardware-GS shadow access. Reused from Celosia's GSHwBackend contract:
    // the CPU backend remains authoritative for GS memory/transfers while a
    // GPU renderer owns raster targets and synchronizes them by 8 KiB pages.
    std::mutex &vramMutex() { return m_mutex; }
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    const std::vector<uint32_t> *textureForHw(const GSDrawState &state, uint32_t &width, uint32_t &height, uint64_t &epoch);
    uint64_t newestWrite(uint64_t begin, uint64_t end) const;
    void noteExternalWrite(uint64_t begin, uint64_t end);
    void fillPage(uint32_t page, uint32_t psm, uint32_t color);

private:
    void ResetUnlocked();
    void UploadImageUnlocked(const uint8_t *data, uint32_t sizeBytes);
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void MarkHwRangeWritten(uint64_t begin, uint64_t end);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t ConvertTexel(const GSDrawState &state, uint32_t raw);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;

    struct HwTextureKey
    {
        uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0;
        uint32_t cpsm = 0, csm = 0, csa = 0;
        uint32_t texa = 0;
        uint64_t clutHash = 0;
        bool operator==(const HwTextureKey &) const = default;
    };
    struct HwTextureKeyHash
    {
        size_t operator()(const HwTextureKey &key) const noexcept;
    };
    struct HwDecodedTexture
    {
        std::vector<uint32_t> texels;
        uint32_t width = 0, height = 0;
        uint64_t version = 0;
    };
    std::unordered_map<HwTextureKey, HwDecodedTexture, HwTextureKeyHash> m_hwTextures;
    std::array<uint64_t, 512> m_hwPageEpoch{};
    uint64_t m_hwEpoch = 1u;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    GSUpload24State m_upload24{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
