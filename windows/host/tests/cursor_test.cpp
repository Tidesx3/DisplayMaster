#include <gtest/gtest.h>

#include "capture/cursor_compositor.h"

using namespace dm;

namespace {
DXGI_OUTDUPL_POINTER_SHAPE_INFO shape(UINT type, UINT w, UINT h, UINT pitch) {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO s{};
    s.Type = type;
    s.Width = w;
    s.Height = h;
    s.Pitch = pitch;
    return s;
}
}  // namespace

TEST(CursorShape, MonochromeAndXorTruthTable) {
    // 8x1 cursor: AND mask row then XOR mask row (Height is 2 rows for monochrome).
    // Pixels 0..3 cover the AND/XOR combinations.
    //   AND=0 XOR=0 -> black, AND=0 XOR=1 -> white, AND=1 XOR=0 -> transparent, AND=1 XOR=1 -> invert
    const std::vector<uint8_t> buf = {0b0011'1111, 0b0101'0000};
    std::vector<uint32_t> color, invert;
    uint32_t w = 0, h = 0;
    CursorCompositor::convert_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 8, 2, 1), buf, color, invert, w, h);
    ASSERT_EQ(w, 8u);
    ASSERT_EQ(h, 1u);
    EXPECT_EQ(color[0], 0xFF000000u);
    EXPECT_EQ(color[1], 0xFFFFFFFFu);
    EXPECT_EQ(color[2], 0u);
    EXPECT_EQ(invert[2], 0xFF000000u);  // black in the invert mask = no inversion
    EXPECT_EQ(color[3], 0u);
    EXPECT_EQ(invert[3], 0xFFFFFFFFu);  // inverted pixel
}

TEST(CursorShape, ColorIsPremultiplied) {
    const uint32_t px[2] = {0x80FF0000u /* 50% red */, 0xFF00FF00u /* opaque green */};
    std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + 8);
    std::vector<uint32_t> color, invert;
    uint32_t w = 0, h = 0;
    CursorCompositor::convert_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 1, 8), buf, color, invert, w, h);
    EXPECT_EQ(color[0], 0x80800000u);
    EXPECT_EQ(color[1], 0xFF00FF00u);
}

TEST(CursorShape, MaskedColor) {
    const uint32_t px[3] = {0x00123456u /* replace */, 0xFF000000u /* xor black: no-op */, 0xFFFFFFFFu /* xor: invert */};
    std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + 12);
    std::vector<uint32_t> color, invert;
    uint32_t w = 0, h = 0;
    CursorCompositor::convert_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 3, 1, 12), buf, color, invert, w, h);
    EXPECT_EQ(color[0], 0xFF123456u);
    EXPECT_EQ(color[1], 0u);
    EXPECT_EQ(invert[1], 0xFF000000u);
    EXPECT_EQ(invert[2], 0xFFFFFFFFu);
}

// Renders through the real D3D11 path on WARP (no GPU needed) and reads pixels back.
class CursorDrawTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)));
        D3D11_TEXTURE2D_DESC td{kW, kH, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0};
        std::vector<uint32_t> gray(kW * kH, 0xFF404040u);
        D3D11_SUBRESOURCE_DATA init{gray.data(), kW * 4, 0};
        ASSERT_TRUE(SUCCEEDED(dev->CreateTexture2D(&td, &init, &target)));
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ASSERT_TRUE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging)));
        ASSERT_TRUE(cursor.init(dev.Get()));
    }
    uint32_t pixel(uint32_t x, uint32_t y) {
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        const uint32_t v = static_cast<const uint32_t*>(m.pData)[y * (m.RowPitch / 4) + x];
        ctx->Unmap(staging.Get(), 0);
        return v & 0x00FFFFFFu;  // compare RGB only
    }
    static constexpr uint32_t kW = 64, kH = 32;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> target, staging;
    CursorCompositor cursor;
};

TEST_F(CursorDrawTest, DrawsOpaqueAndBlendedPixelsAtPosition) {
    // 2x1 color cursor: opaque red, fully transparent.
    const uint32_t px[2] = {0xFFFF0000u, 0x00000000u};
    std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + 8);
    cursor.set_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 1, 8), buf);
    cursor.set_position(true, POINT{10, 5});
    cursor.draw(target.Get(), kW, kH);
    EXPECT_EQ(pixel(10, 5), 0xFF0000u);  // cursor pixel
    EXPECT_EQ(pixel(11, 5), 0x404040u);  // transparent: desktop shows through
    EXPECT_EQ(pixel(9, 5), 0x404040u);   // outside the cursor
    EXPECT_EQ(pixel(10, 6), 0x404040u);
}

TEST_F(CursorDrawTest, MonochromeInvertPixelsInvertTheDesktop) {
    // 8x1 monochrome: pixel 0 inverts (AND=1, XOR=1), others transparent (AND=1, XOR=0).
    const std::vector<uint8_t> buf = {0xFF, 0x80};
    cursor.set_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 8, 2, 1), buf);
    cursor.set_position(true, POINT{0, 0});
    cursor.draw(target.Get(), kW, kH);
    EXPECT_EQ(pixel(0, 0), 0xBFBFBFu);  // 0x40 inverted
    EXPECT_EQ(pixel(1, 0), 0x404040u);
}

TEST_F(CursorDrawTest, HiddenCursorDrawsNothing) {
    const uint32_t px[1] = {0xFFFF0000u};
    std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + 4);
    cursor.set_shape(shape(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 1, 1, 4), buf);
    cursor.set_position(false, POINT{3, 3});
    cursor.draw(target.Get(), kW, kH);
    EXPECT_EQ(pixel(3, 3), 0x404040u);
}
