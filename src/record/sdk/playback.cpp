#include <ctime>
#include <iostream>
#include <sstream>

#include <k4a/k4a.h>
#include <k4arecord/playback.h>
#include <k4ainternal/matroska_read.h>
#include <k4ainternal/logging.h>
#include <k4ainternal/common.h>

using namespace k4arecord;
using namespace LIBMATROSKA_NAMESPACE;

k4a_result_t k4a_playback_open(const char *path, k4a_playback_t *playback_handle)
{
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, path == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, playback_handle == NULL);
    k4a_playback_context_t *context = NULL;
    logger_t logger_handle = NULL;
    k4a_result_t result = K4A_RESULT_SUCCEEDED;

    // Instantiate the logger as early as possible
    logger_config_t logger_config;
    logger_config_init_default(&logger_config);
    result = TRACE_CALL(logger_create(&logger_config, &logger_handle));

    if (K4A_SUCCEEDED(result))
    {
        context = k4a_playback_t_create(playback_handle);
        result = K4A_RESULT_FROM_BOOL(context != NULL);
    }

    if (K4A_SUCCEEDED(result))
    {
        context->logger_handle = logger_handle;
        context->file_path = path;

        try
        {
            context->ebml_file = make_unique<LargeFileIOCallback>(path, MODE_READ);
            context->stream = make_unique<libebml::EbmlStream>(*context->ebml_file);
        }
        catch (std::ios_base::failure e)
        {
            logger_error(LOGGER_RECORD, "Unable to open file '%s': %s", path, e.what());
            result = K4A_RESULT_FAILED;
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        result = TRACE_CALL(parse_mkv(context));
    }

    if (K4A_SUCCEEDED(result))
    {
        // Seek to the first cluster
        context->seek_cluster = find_cluster(context, context->first_cluster_offset, 0);
        if (context->seek_cluster == nullptr)
        {
            logger_error(LOGGER_RECORD, "Failed to parse recording, recording is empty.");
            result = K4A_RESULT_FAILED;
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        reset_seek_pointers(context, 0);
    }
    else
    {
        if (context && context->ebml_file)
        {
            try
            {
                context->ebml_file->close();
            }
            catch (std::ios_base::failure e)
            {
                // The file was opened as read-only, ignore any close failures.
            }
        }

        if (logger_handle)
        {
            logger_destroy(logger_handle);
        }
        k4a_playback_t_destroy(*playback_handle);
        *playback_handle = NULL;
    }

    return result;
}

k4a_buffer_result_t k4a_playback_get_raw_calibration(k4a_playback_t playback_handle, uint8_t *data, size_t *data_size)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_BUFFER_RESULT_FAILED, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_BUFFER_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_BUFFER_RESULT_FAILED, data_size == NULL);

    KaxFileData &file_data = GetChild<KaxFileData>(*context->calibration_attachment);
    // Attachment is stored in binary, not a string, so null termination is not guaranteed.
    // Check if the binary data is null terminated, and if not append a zero.
    size_t append_zero = 0;
    assert(file_data.GetSize() > 0 && file_data.GetSize() <= SIZE_MAX);
    if (file_data.GetBuffer()[file_data.GetSize() - 1] != 0)
    {
        append_zero = 1;
    }
    if (data != NULL && *data_size >= (file_data.GetSize() + append_zero))
    {
        memcpy(data, static_cast<uint8_t *>(file_data.GetBuffer()), (size_t)file_data.GetSize());
        if (append_zero)
        {
            data[file_data.GetSize()] = 0;
        }
        *data_size = (size_t)file_data.GetSize() + append_zero;
        return K4A_BUFFER_RESULT_SUCCEEDED;
    }
    else
    {
        *data_size = (size_t)file_data.GetSize() + append_zero;
        return K4A_BUFFER_RESULT_TOO_SMALL;
    }
}

k4a_result_t k4a_playback_get_calibration(k4a_playback_t playback_handle, k4a_calibration_t *calibration)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context->calibration_attachment == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, calibration == NULL);

    if (context->device_calibration == nullptr)
    {
        context->device_calibration = make_unique<k4a_calibration_t>();
        KaxFileData &file_data = GetChild<KaxFileData>(*context->calibration_attachment);
        // Attachment is stored in binary, not a string, so null termination is not guaranteed.
        assert(file_data.GetSize() <= SIZE_MAX);
        std::vector<char> buffer = std::vector<char>((size_t)file_data.GetSize() + 1);
        memcpy(&buffer[0], file_data.GetBuffer(), (size_t)file_data.GetSize());
        buffer[buffer.size() - 1] = 0;
        k4a_result_t result = k4a_calibration_get_from_raw(buffer.data(),
                                                           buffer.size(),
                                                           context->depth_mode,
                                                           context->color_resolution,
                                                           context->device_calibration.get());
        if (K4A_FAILED(result))
        {
            context->device_calibration.reset();
            return result;
        }
    }
    *calibration = *context->device_calibration;

    return K4A_RESULT_SUCCEEDED;
}

k4a_result_t k4a_playback_get_color_format(k4a_playback_t playback_handle, k4a_image_format_t *format)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, format == NULL);

    if (context->color_resolution != K4A_COLOR_RESOLUTION_OFF)
    {
        *format = context->color_format;
        return K4A_RESULT_SUCCEEDED;
    }
    return K4A_RESULT_FAILED;
}

k4a_color_resolution_t k4a_playback_get_color_resolution(k4a_playback_t playback_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_COLOR_RESOLUTION_OFF, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_COLOR_RESOLUTION_OFF, context == NULL);

    return context->color_resolution;
}

k4a_depth_mode_t k4a_playback_get_depth_mode(k4a_playback_t playback_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_DEPTH_MODE_OFF, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_DEPTH_MODE_OFF, context == NULL);

    return context->depth_mode;
}

k4a_fps_t k4a_playback_get_camera_fps(k4a_playback_t playback_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_FRAMES_PER_SECOND_30, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_FRAMES_PER_SECOND_30, context == NULL);

    if (context->color_resolution != K4A_COLOR_RESOLUTION_OFF || context->depth_mode != K4A_DEPTH_MODE_OFF)
    {
        return context->camera_fps;
    }
    return K4A_FRAMES_PER_SECOND_30;
}

const char *k4a_playback_get_tag(k4a_playback_t playback_handle, const char *name)
{
    RETURN_VALUE_IF_HANDLE_INVALID(NULL, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(NULL, context == NULL);

    KaxTag *tag = get_tag(context, name);
    return tag ? get_tag_string(tag).c_str() : NULL;
}

k4a_stream_result_t k4a_playback_get_next_capture(k4a_playback_t playback_handle, k4a_capture_t *capture_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_STREAM_RESULT_FAILED, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_STREAM_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_STREAM_RESULT_FAILED, capture_handle == NULL);

    return get_capture(context, capture_handle, true);
}

k4a_stream_result_t k4a_playback_get_previous_capture(k4a_playback_t playback_handle, k4a_capture_t *capture_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_STREAM_RESULT_FAILED, k4a_playback_t, playback_handle);
    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_STREAM_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_STREAM_RESULT_FAILED, capture_handle == NULL);

    return get_capture(context, capture_handle, false);
}

k4a_stream_result_t k4a_playback_get_next_imu_sample(k4a_playback_t playback_handle, k4a_imu_sample_t *imu_sample);

k4a_stream_result_t k4a_playback_get_previous_imu_sample(k4a_playback_t playback_handle, k4a_imu_sample_t *imu_sample);

k4a_result_t k4a_playback_seek_timestamp(k4a_playback_t playback_handle,
                                         int64_t offset_usec,
                                         k4a_playback_seek_origin_t origin)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_playback_t, playback_handle);

    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context->segment == nullptr);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, origin != K4A_PLAYBACK_SEEK_BEGIN && origin != K4A_PLAYBACK_SEEK_END);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, origin == K4A_PLAYBACK_SEEK_BEGIN && offset_usec < 0);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, origin == K4A_PLAYBACK_SEEK_END && offset_usec > 0);

    // TODO Test each seek mode
    uint64_t target_time_ns = origin == K4A_PLAYBACK_SEEK_END ? (context->last_timestamp_ns + 1) : 0;

    // TODO: Figure out a better way to handle this.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
    target_time_ns += offset_usec * 1000;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    k4a_result_t result = K4A_RESULT_SUCCEEDED;

    std::shared_ptr<KaxCluster> seek_cluster = seek_timestamp(context, target_time_ns);
    result = K4A_RESULT_FROM_BOOL(seek_cluster != nullptr);

    if (K4A_SUCCEEDED(result))
    {
        context->seek_cluster = seek_cluster;
        reset_seek_pointers(context, target_time_ns);
    }

    return result;
}

uint64_t k4a_playback_get_last_timestamp_usec(k4a_playback_t playback_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(0, k4a_playback_t, playback_handle);

    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    RETURN_VALUE_IF_ARG(0, context == NULL);
    return context->last_timestamp_ns / 1000;
}

void k4a_playback_close(const k4a_playback_t playback_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(VOID_VALUE, k4a_playback_t, playback_handle);

    k4a_playback_context_t *context = k4a_playback_t_get_context(playback_handle);
    if (context != NULL)
    {
        try
        {
            context->ebml_file->close();
        }
        catch (std::ios_base::failure e)
        {
            // The file was opened as read-only, ignore any close failures.
        }

        // After this destroy, logging will no longer happen.
        if (context->logger_handle)
        {
            logger_destroy(context->logger_handle);
        }
    }
    k4a_playback_t_destroy(playback_handle);
}