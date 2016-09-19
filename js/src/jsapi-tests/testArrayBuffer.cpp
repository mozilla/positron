/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 */

#include "jsfriendapi.h"

#include "jsapi-tests/tests.h"

BEGIN_TEST(testArrayBuffer_bug720949_steal)
{
    static const unsigned NUM_TEST_BUFFERS  = 2;
    static const unsigned MAGIC_VALUE_1 = 3;
    static const unsigned MAGIC_VALUE_2 = 17;

    JS::RootedObject buf_len1(cx), buf_len200(cx);
    JS::RootedObject tarray_len1(cx), tarray_len200(cx);

    uint32_t sizes[NUM_TEST_BUFFERS] = { sizeof(uint32_t), 200 * sizeof(uint32_t) };
    JS::HandleObject testBuf[NUM_TEST_BUFFERS] = { buf_len1, buf_len200 };
    JS::HandleObject testArray[NUM_TEST_BUFFERS] = { tarray_len1, tarray_len200 };

    // Single-element ArrayBuffer (uses fixed slots for storage)
    CHECK(buf_len1 = JS_NewArrayBuffer(cx, sizes[0]));
    CHECK(tarray_len1 = JS_NewInt32ArrayWithBuffer(cx, testBuf[0], 0, -1));

    JS_SetElement(cx, testArray[0], 0, MAGIC_VALUE_1);

    // Many-element ArrayBuffer (uses dynamic storage)
    CHECK(buf_len200 = JS_NewArrayBuffer(cx, 200 * sizeof(uint32_t)));
    CHECK(tarray_len200 = JS_NewInt32ArrayWithBuffer(cx, testBuf[1], 0, -1));

    for (unsigned i = 0; i < NUM_TEST_BUFFERS; i++) {
        JS::HandleObject obj = testBuf[i];
        JS::HandleObject view = testArray[i];
        uint32_t size = sizes[i];
        JS::RootedValue v(cx);

        // Byte lengths should all agree
        CHECK(JS_IsArrayBufferObject(obj));
        CHECK_EQUAL(JS_GetArrayBufferByteLength(obj), size);
        JS_GetProperty(cx, obj, "byteLength", &v);
        CHECK(v.isInt32(size));
        JS_GetProperty(cx, view, "byteLength", &v);
        CHECK(v.isInt32(size));

        // Modifying the underlying data should update the value returned through the view
        {
            JS::AutoCheckCannotGC nogc;
            bool sharedDummy;
            uint8_t* data = JS_GetArrayBufferData(obj, &sharedDummy, nogc);
            CHECK(data != nullptr);
            *reinterpret_cast<uint32_t*>(data) = MAGIC_VALUE_2;
        }
        CHECK(JS_GetElement(cx, view, 0, &v));
        CHECK(v.isInt32(MAGIC_VALUE_2));

        // Steal the contents
        void* contents = JS_StealArrayBufferContents(cx, obj);
        CHECK(contents != nullptr);

        CHECK(JS_IsDetachedArrayBufferObject(obj));

        // Transfer to a new ArrayBuffer
        JS::RootedObject dst(cx, JS_NewArrayBufferWithContents(cx, size, contents));
        CHECK(JS_IsArrayBufferObject(dst));
        {
            JS::AutoCheckCannotGC nogc;
            bool sharedDummy;
            (void) JS_GetArrayBufferData(obj, &sharedDummy, nogc);
        }

        JS::RootedObject dstview(cx, JS_NewInt32ArrayWithBuffer(cx, dst, 0, -1));
        CHECK(dstview != nullptr);

        CHECK_EQUAL(JS_GetArrayBufferByteLength(dst), size);
        {
            JS::AutoCheckCannotGC nogc;
            bool sharedDummy;
            uint8_t* data = JS_GetArrayBufferData(dst, &sharedDummy, nogc);
            CHECK(data != nullptr);
            CHECK_EQUAL(*reinterpret_cast<uint32_t*>(data), MAGIC_VALUE_2);
        }
        CHECK(JS_GetElement(cx, dstview, 0, &v));
        CHECK(v.isInt32(MAGIC_VALUE_2));
    }

    return true;
}
END_TEST(testArrayBuffer_bug720949_steal)

// Varying number of views of a buffer, to test the detachment weak pointers
BEGIN_TEST(testArrayBuffer_bug720949_viewList)
{
    JS::RootedObject buffer(cx);

    // No views
    buffer = JS_NewArrayBuffer(cx, 2000);
    buffer = nullptr;
    GC(cx);

    // One view.
    {
        buffer = JS_NewArrayBuffer(cx, 2000);
        JS::RootedObject view(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, -1));
        void* contents = JS_StealArrayBufferContents(cx, buffer);
        CHECK(contents != nullptr);
        JS_free(nullptr, contents);
        GC(cx);
        CHECK(hasDetachedBuffer(view));
        CHECK(JS_IsDetachedArrayBufferObject(buffer));
        view = nullptr;
        GC(cx);
        buffer = nullptr;
        GC(cx);
    }

    // Two views
    {
        buffer = JS_NewArrayBuffer(cx, 2000);

        JS::RootedObject view1(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, -1));
        JS::RootedObject view2(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 1, 200));

        // Remove, re-add a view
        view2 = nullptr;
        GC(cx);
        view2 = JS_NewUint8ArrayWithBuffer(cx, buffer, 1, 200);

        // Detach
        void* contents = JS_StealArrayBufferContents(cx, buffer);
        CHECK(contents != nullptr);
        JS_free(nullptr, contents);

        CHECK(hasDetachedBuffer(view1));
        CHECK(hasDetachedBuffer(view2));
        CHECK(JS_IsDetachedArrayBufferObject(buffer));

        view1 = nullptr;
        GC(cx);
        view2 = nullptr;
        GC(cx);
        buffer = nullptr;
        GC(cx);
    }

    return true;
}

static void GC(JSContext* cx)
{
    JS_GC(cx);
    JS_GC(cx); // Trigger another to wait for background finalization to end
}

bool hasDetachedBuffer(JS::HandleObject obj) {
    JS::RootedValue v(cx);
    return JS_GetProperty(cx, obj, "byteLength", &v) && v.toInt32() == 0;
}

END_TEST(testArrayBuffer_bug720949_viewList)

BEGIN_TEST(testArrayBuffer_externalize)
{
    if (!testWithSize(cx, 2))    // inlined storage
        return false;
    if (!testWithSize(cx, 2000)) // externalized storage
        return false;

    return true;
}

bool testWithSize(JSContext* cx, int32_t n)
{
    JS::RootedObject buffer(cx);

    buffer = JS_NewArrayBuffer(cx, n);
    JS::RootedObject view(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, -1));
    void* contents = JS_ExternalizeArrayBufferContents(cx, buffer);
    CHECK(contents != nullptr);
    CHECK(hasExpectedLength(view, n));
    CHECK(!JS_IsDetachedArrayBufferObject(buffer));
    CHECK(JS_GetArrayBufferByteLength(buffer) == uint32_t(n));
    view = nullptr;
    GC(cx);
    buffer = nullptr;
    GC(cx);
    JS_free(nullptr, contents);
    GC(cx);

    return true;
}

static void GC(JSContext* cx)
{
    JS_GC(cx);
    JS_GC(cx); // Trigger another to wait for background finalization to end
}

bool hasExpectedLength(JS::HandleObject obj, int32_t n) {
    JS::RootedValue v(cx);
    return JS_GetProperty(cx, obj, "byteLength", &v) && v.toInt32() == n;
}

END_TEST(testArrayBuffer_externalize)
