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
