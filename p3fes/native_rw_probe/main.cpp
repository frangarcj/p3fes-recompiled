#include <rw.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::uint32_t readLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t findRwOffset(const std::vector<std::uint8_t> &data)
{
    const std::size_t scanEnd = std::min<std::size_t>(data.size(), 0x100);
    for (std::size_t offset = 0; offset + 12 <= scanEnd; ++offset)
    {
        const std::uint32_t id = readLe32(data.data() + offset);
        const std::uint32_t size = readLe32(data.data() + offset + 4);
        if ((id == rw::ID_TEXDICTIONARY || id == rw::ID_CLUMP) &&
            size <= data.size() - offset - 12)
            return offset;
    }
    return data.size();
}

std::vector<std::uint8_t> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

rw::TexDictionary *readTextureDictionary(std::uint8_t *data, std::size_t size)
{
    rw::StreamMemory stream;
    stream.open(data, static_cast<rw::uint32>(size));
    rw::TexDictionary *dict = nullptr;
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr))
        dict = rw::TexDictionary::streamRead(&stream);
    stream.close();
    return dict;
}

rw::Clump *readClump(std::uint8_t *data, std::size_t size)
{
    rw::StreamMemory stream;
    stream.open(data, static_cast<rw::uint32>(size));
    rw::Clump *clump = nullptr;
    if (rw::findChunk(&stream, rw::ID_CLUMP, nullptr, nullptr))
        clump = rw::Clump::streamRead(&stream);
    stream.close();
    return clump;
}

void printClump(rw::Clump *clump)
{
    int atomics = 0;
    int nativeGeometries = 0;
    FORLIST(lnk, clump->atomics)
    {
        rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
        rw::Geometry *geometry = atomic->geometry;
        if (geometry == nullptr)
            continue;

        const bool native = (geometry->flags & rw::Geometry::NATIVE) != 0;
        if (native)
            ++nativeGeometries;

        rw::Skin *skin = rw::Skin::get(geometry);
        std::printf(
            "atomic %d: vertices=%d triangles=%d materials=%d flags=0x%08X native=%s "
            "instPlatform=%d skinBones=%d\n",
            atomics,
            geometry->numVertices,
            geometry->numTriangles,
            geometry->matList.numMaterials,
            static_cast<unsigned>(geometry->flags),
            native ? "yes" : "no",
            geometry->instData ? geometry->instData->platform : -1,
            skin ? skin->numBones : 0);
        ++atomics;
    }

    std::printf("summary: atomics=%d nativeGeometries=%d\n", atomics, nativeGeometries);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: %s MODEL.RMD\n", argv[0]);
        return 2;
    }

    std::vector<std::uint8_t> file = readFile(argv[1]);
    if (file.empty())
    {
        std::fprintf(stderr, "Could not read %s\n", argv[1]);
        return 1;
    }

    const std::size_t rwOffset = findRwOffset(file);
    if (rwOffset == file.size())
    {
        std::fprintf(stderr, "No RenderWare stream found near the start of %s\n", argv[1]);
        return 1;
    }
    std::printf("RenderWare stream offset: 0x%zX\n", rwOffset);

    rw::Engine::init();
    rw::registerHAnimPlugin();
    rw::registerSkinPlugin();
    rw::registerMatFXPlugin();
    rw::ps2::registerPlatformPlugins();
    rw::ps2::registerNativeDataPlugin();
    rw::ps2::registerADCPlugin();

    if (!rw::Engine::open(nullptr) || !rw::Engine::start())
    {
        std::fprintf(stderr, "Could not start librw\n");
        return 1;
    }

    std::uint8_t *rwData = file.data() + rwOffset;
    const std::size_t rwSize = file.size() - rwOffset;

    rw::TexDictionary *dictionary = readTextureDictionary(rwData, rwSize);
    if (dictionary)
        rw::TexDictionary::setCurrent(dictionary);

    rw::Clump *clump = readClump(rwData, rwSize);
    if (clump == nullptr)
    {
        std::fprintf(stderr, "librw could not read the RMD clump\n");
        rw::Engine::stop();
        rw::Engine::close();
        rw::Engine::term();
        return 1;
    }

    printClump(clump);
    clump->destroy();
    if (dictionary)
        dictionary->destroy();

    rw::Engine::stop();
    rw::Engine::close();
    rw::Engine::term();
    return 0;
}
