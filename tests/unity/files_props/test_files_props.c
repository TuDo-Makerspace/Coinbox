/*
 *	The MIT License (MIT)
 *
 *	Copyright (c) 2026 TuDo Makerspace
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy
 *	of this software and associated documentation files (the "Software"), to deal
 *	in the Software without restriction, including without limitation the rights
 *	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *	copies of the Software, and to permit persons to whom the Software is
 *	furnished to do so, subject to the following conditions:
 *
 *	The above copyright notice and this permission notice shall be included in all
 *	copies or substantial portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *	SOFTWARE.
 */

#include "unity.h"

#include "files.h"

TEST_CASE("files_props_init sets basename and defaults", "[files]")
{
    file_properties_t props;
    files_props_init(&props, "/data/sounds/coin.mp3");

    TEST_ASSERT_EQUAL_STRING("coin.mp3", props.name);
    TEST_ASSERT_EQUAL_UINT8(0, props.probability);
    TEST_ASSERT_EQUAL_UINT8(0, props.volume);
    TEST_ASSERT_FALSE(props.enabled);
}

TEST_CASE("files_props_set clamps probability and volume", "[files]")
{
    file_properties_t props;
    files_props_init(&props, "placeholder.mp3");

    files_props_set(&props, "/tmp/oneup.mp3", 255, 255, true);

    TEST_ASSERT_EQUAL_STRING("oneup.mp3", props.name);
    TEST_ASSERT_EQUAL_UINT8(FILE_PROBABILITY_MAX, props.probability);
    TEST_ASSERT_EQUAL_UINT8(FILE_VOLUME_MAX, props.volume);
    TEST_ASSERT_TRUE(props.enabled);
}

TEST_CASE("files_props_set accepts disabled zeroed entry", "[files]")
{
    file_properties_t props;
    files_props_init(&props, "placeholder.mp3");

    files_props_set(&props, "coin.mp3", 0, 0, false);

    TEST_ASSERT_EQUAL_STRING("coin.mp3", props.name);
    TEST_ASSERT_EQUAL_UINT8(0, props.probability);
    TEST_ASSERT_EQUAL_UINT8(0, props.volume);
    TEST_ASSERT_FALSE(props.enabled);
}
