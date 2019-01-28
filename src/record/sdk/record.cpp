#include <ctime>
#include <iostream>
#include <sstream>

#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <k4ainternal/matroska_write.h>
#include <k4ainternal/logging.h>
#include <k4ainternal/common.h>

using namespace k4arecord;
using namespace LIBMATROSKA_NAMESPACE;

k4a_result_t k4a_record_create(const char *path,
                               k4a_device_t device,
                               const k4a_device_configuration_t device_config,
                               k4a_record_t *recording_handle)
{
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, path == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, recording_handle == NULL);
    k4a_record_context_t *context = NULL;
    logger_t logger_handle = NULL;
    k4a_result_t result = K4A_RESULT_SUCCEEDED;

    // Instantiate the logger as early as possible
    logger_config_t logger_config;
    logger_config_init_default(&logger_config);
    result = TRACE_CALL(logger_create(&logger_config, &logger_handle));

    if (K4A_SUCCEEDED(result))
    {
        context = k4a_record_t_create(recording_handle);
        result = K4A_RESULT_FROM_BOOL(context != NULL);
    }

    if (K4A_SUCCEEDED(result))
    {
        context->logger_handle = logger_handle;
        context->file_path = path;

        try
        {
            context->ebml_file = make_unique<LargeFileIOCallback>(path, MODE_CREATE);
        }
        catch (std::ios_base::failure e)
        {
            logger_error(LOGGER_RECORD, "Unable to open file '%s': %s", path, e.what());
            result = K4A_RESULT_FAILED;
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        context->device = device;
        context->device_config = device_config;
        context->pending_clusters = make_unique<std::list<cluster_t *>>();
        context->imu_buffer = make_unique<std::vector<uint8_t>>();

        context->pending_cluster_lock = Lock_Init();

        context->timecode_scale = MATROSKA_TIMESCALE_NS;
        context->camera_fps = k4a_convert_fps_to_uint(device_config.camera_fps);
        if (context->camera_fps == 0)
        {
            // Set camera FPS to 30 if no cameras are enabled so IMU can still be written.
            context->camera_fps = 30;
        }
    }

    uint32_t color_width = 0;
    uint32_t color_height = 0;
    if (K4A_SUCCEEDED(result))
    {
        if (!k4a_convert_resolution_to_width_height(device_config.color_resolution, &color_width, &color_height))
        {
            logger_error(LOGGER_RECORD,
                         "Unsupported color_resolution specified in recording: %d",
                         device_config.color_resolution);
            result = K4A_RESULT_FAILED;
        }
    }

    std::ostringstream color_mode_str;
    if (K4A_SUCCEEDED(result))
    {
        context->color_format = device_config.color_format;
        if (device_config.color_resolution != K4A_COLOR_RESOLUTION_OFF)
        {
            switch (context->color_format)
            {
            case K4A_IMAGE_FORMAT_COLOR_NV12:
                color_mode_str << "NV12_" << color_height << "P";
                break;
            case K4A_IMAGE_FORMAT_COLOR_YUY2:
                color_mode_str << "YUY2_" << color_height << "P";
                break;
            case K4A_IMAGE_FORMAT_COLOR_MJPG:
                color_mode_str << "MJPG_" << color_height << "P";
                break;
            default:
                logger_error(LOGGER_RECORD,
                             "Unsupported color_format specified in recording: %d",
                             context->color_format);
                result = K4A_RESULT_FAILED;
            }
        }
        else
        {
            color_mode_str << "OFF";
        }
    }

    const char *depth_mode_str = "OFF";
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
    if (K4A_SUCCEEDED(result))
    {
        if (device_config.depth_mode != K4A_DEPTH_MODE_OFF)
        {
            for (size_t i = 0; i < arraysize(depth_modes); i++)
            {
                if (device_config.depth_mode == depth_modes[i].first)
                {
                    if (!k4a_convert_depth_mode_to_width_height(depth_modes[i].first, &depth_width, &depth_height))
                    {
                        logger_error(LOGGER_RECORD,
                                     "Unsupported depth_mode specified in recording: %d",
                                     device_config.depth_mode);
                        result = K4A_RESULT_FAILED;
                    }
                    depth_mode_str = depth_modes[i].second.c_str();
                    break;
                }
            }
            if (depth_width == 0 || depth_height == 0)
            {
                logger_error(LOGGER_RECORD,
                             "Unsupported depth_mode specified in recording: %d",
                             device_config.depth_mode);
                result = K4A_RESULT_FAILED;
            }
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        context->file_segment = make_unique<KaxSegment>();

        { // Setup segment info
            auto &segment_info = GetChild<KaxInfo>(*context->file_segment);

            GetChild<KaxTimecodeScale>(segment_info).SetValue(context->timecode_scale);
            GetChild<KaxMuxingApp>(segment_info).SetValue(L"libmatroska-1.4.9");
            std::ostringstream version_str;
            version_str << "k4arecord-" << K4A_VERSION_STR;
            GetChild<KaxWritingApp>(segment_info).SetValueUTF8(version_str.str());
            GetChild<KaxDateUTC>(segment_info).SetEpochDate(time(0));
            GetChild<KaxTitle>(segment_info).SetValue(L"Azure Kinect");
        }

        auto &tags = GetChild<KaxTags>(*context->file_segment);
        tags.EnableChecksum();
    }

    if (K4A_SUCCEEDED(result) && device_config.color_resolution != K4A_COLOR_RESOLUTION_OFF)
    {
        BITMAPINFOHEADER codec_info = {};
        result = TRACE_CALL(populate_bitmap_info_header(&codec_info, color_width, color_height, context->color_format));

        context->color_track = add_track(context,
                                         "COLOR",
                                         track_video,
                                         "V_MS/VFW/FOURCC",
                                         reinterpret_cast<uint8_t *>(&codec_info),
                                         sizeof(codec_info));
        set_track_info_video(context->color_track, color_width, color_height, context->camera_fps);

        uint64_t track_uid = GetChild<KaxTrackUID>(*context->color_track).GetValue();
        add_tag(context, "K4A_COLOR_MODE", color_mode_str.str().c_str(), TAG_TARGET_TYPE_TRACK, track_uid);
    }

    if (K4A_SUCCEEDED(result))
    {
        if (device_config.depth_mode == K4A_DEPTH_MODE_PASSIVE_IR)
        {
            add_tag(context, "K4A_DEPTH_MODE", depth_mode_str);
        }
        else if (device_config.depth_mode != K4A_DEPTH_MODE_OFF)
        {
            // Depth track
            BITMAPINFOHEADER codec_info = {};
            result = TRACE_CALL(
                populate_bitmap_info_header(&codec_info, depth_width, depth_height, K4A_IMAGE_FORMAT_DEPTH16));

            context->depth_track = add_track(context,
                                             "DEPTH",
                                             track_video,
                                             "V_MS/VFW/FOURCC",
                                             reinterpret_cast<uint8_t *>(&codec_info),
                                             sizeof(codec_info));
            set_track_info_video(context->depth_track, depth_width, depth_height, context->camera_fps);

            uint64_t track_uid = GetChild<KaxTrackUID>(*context->depth_track).GetValue();
            add_tag(context, "K4A_DEPTH_MODE", depth_mode_str, TAG_TARGET_TYPE_TRACK, track_uid);
        }
    }

    if (K4A_SUCCEEDED(result) && device_config.depth_mode != K4A_DEPTH_MODE_OFF)
    {
        // IR Track
        BITMAPINFOHEADER codec_info = {};
        result = TRACE_CALL(populate_bitmap_info_header(&codec_info, depth_width, depth_height, K4A_IMAGE_FORMAT_IR16));

        context->ir_track = add_track(context,
                                      "IR",
                                      track_video,
                                      "V_MS/VFW/FOURCC",
                                      reinterpret_cast<uint8_t *>(&codec_info),
                                      sizeof(codec_info));
        set_track_info_video(context->ir_track, depth_width, depth_height, context->camera_fps);

        uint64_t track_uid = GetChild<KaxTrackUID>(*context->ir_track).GetValue();
        add_tag(context,
                "K4A_IR_MODE",
                device_config.depth_mode == K4A_DEPTH_MODE_PASSIVE_IR ? "PASSIVE" : "ACTIVE",
                TAG_TARGET_TYPE_TRACK,
                track_uid);
    }

    if (K4A_SUCCEEDED(result))
    {
        // Add the firmware version and device serial number to the recording
        k4a_hardware_version_t version_info;
        result = TRACE_CALL(k4a_device_get_version(device, &version_info));

        std::ostringstream color_firmware_str;
        color_firmware_str << version_info.rgb.major << "." << version_info.rgb.minor << "."
                           << version_info.rgb.iteration;
        std::ostringstream depth_firmware_str;
        depth_firmware_str << version_info.depth.major << "." << version_info.depth.minor << "."
                           << version_info.depth.iteration;
        add_tag(context, "K4A_COLOR_FIRMWARE_VERSION", color_firmware_str.str().c_str());
        add_tag(context, "K4A_DEPTH_FIRMWARE_VERSION", depth_firmware_str.str().c_str());

        char serial_number_buffer[256];
        size_t serial_number_buffer_size = sizeof(serial_number_buffer);
        // If reading the device serial number fails, just log the error and continue. The recording is still valid.
        if (TRACE_BUFFER_CALL(k4a_device_get_serialnum(device, serial_number_buffer, &serial_number_buffer_size)) ==
            K4A_BUFFER_RESULT_SUCCEEDED)
        {
            add_tag(context, "K4A_DEVICE_SERIAL_NUMBER", serial_number_buffer);
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        // Add calibration.json to the recording
        size_t calibration_size = 0;
        k4a_buffer_result_t buffer_result = TRACE_BUFFER_CALL(
            k4a_device_get_raw_calibration(device, NULL, &calibration_size));
        if (buffer_result == K4A_BUFFER_RESULT_TOO_SMALL)
        {
            std::vector<uint8_t> calibration_buffer = std::vector<uint8_t>(calibration_size);
            buffer_result = TRACE_BUFFER_CALL(
                k4a_device_get_raw_calibration(device, calibration_buffer.data(), &calibration_size));
            if (buffer_result == K4A_BUFFER_RESULT_SUCCEEDED)
            {
                KaxAttached *attached = add_attachment(context,
                                                       "calibration.json",
                                                       "application/octet-stream",
                                                       calibration_buffer.data(),
                                                       calibration_size);
                add_tag(context,
                        "K4A_CALIBRATION_FILE",
                        "calibration.json",
                        TAG_TARGET_TYPE_ATTACHMENT,
                        get_attachment_uid(attached));
            }
            else
            {
                result = K4A_RESULT_FAILED;
            }
        }
        else
        {
            result = K4A_RESULT_FAILED;
        }
    }

    if (K4A_SUCCEEDED(result))
    {
        auto &cues = GetChild<KaxCues>(*context->file_segment);
        cues.SetGlobalTimecodeScale(context->timecode_scale);
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
                // The file is empty at this point, ignore any close failures.
            }
        }

        if (logger_handle)
        {
            logger_destroy(logger_handle);
        }
        k4a_record_t_destroy(*recording_handle);
        *recording_handle = NULL;
    }

    return result;
}

k4a_result_t k4a_record_add_tag(const k4a_record_t recording_handle, const char *name, const char *value)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, name == NULL || value == NULL);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);

    if (context->header_written)
    {
        logger_error(LOGGER_RECORD, "Tags must be added before the recording header is written.");
        return K4A_RESULT_FAILED;
    }

    add_tag(context, name, value);

    return K4A_RESULT_SUCCEEDED;
}

k4a_result_t k4a_record_add_imu_track(const k4a_record_t recording_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);

    if (context->header_written)
    {
        logger_error(LOGGER_RECORD, "The IMU track must be added before the recording header is written.");
        return K4A_RESULT_FAILED;
    }

    if (context->imu_track != NULL)
    {
        logger_error(LOGGER_RECORD, "The IMU track has already been added to this recording.");
        return K4A_RESULT_FAILED;
    }

    context->imu_track = add_track(context, "IMU", track_subtitle, "S_K4A/IMU");

    return K4A_RESULT_SUCCEEDED;
}

k4a_result_t k4a_record_write_header(const k4a_record_t recording_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);

    if (context->header_written)
    {
        logger_error(LOGGER_RECORD, "The header for this recording has already been written.");
        return K4A_RESULT_FAILED;
    }

    try
    {
        // Make sure we're at the beginning of the file in case we're rewriting a file.
        context->ebml_file->setFilePointer(0, libebml::seek_beginning);

        { // Render Ebml header
            EbmlHead file_head;

            GetChild<EDocType>(file_head).SetValue("matroska");
            GetChild<EDocTypeVersion>(file_head).SetValue(MATROSKA_VERSION);
            GetChild<EDocTypeReadVersion>(file_head).SetValue(2);

            file_head.Render(*context->ebml_file, true);
        }

        // Recordings can get very large, so pad the length field up to 8 bytes from the start.
        context->file_segment->WriteHead(*context->ebml_file, 8);

        { // Write void blocks to reserve space for seeking metadata and the segment info so they can be updated at
          // the end
            context->seek_void = make_unique<EbmlVoid>();
            context->seek_void->SetSize(1024);
            context->seek_void->Render(*context->ebml_file);

            context->segment_info_void = make_unique<EbmlVoid>();
            context->segment_info_void->SetSize(256);
            context->segment_info_void->Render(*context->ebml_file);
        }

        { // Write tracks
            auto &tracks = GetChild<KaxTracks>(*context->file_segment);
            tracks.Render(*context->ebml_file);
        }

        { // Write attachments
            auto &attachments = GetChild<KaxAttachments>(*context->file_segment);
            attachments.Render(*context->ebml_file);
        }

        { // Write tags with a void block after to make editing easier
            auto &tags = GetChild<KaxTags>(*context->file_segment);
            tags.Render(*context->ebml_file, true);

            EbmlVoid tag_void;
            tag_void.SetSize(1024);
            tag_void.Render(*context->ebml_file);
        }
    }
    catch (std::ios_base::failure e)
    {
        logger_error(LOGGER_RECORD, "Failed to write recording header '%s': %s", context->file_path, e.what());
        return K4A_RESULT_FAILED;
    }

    RETURN_IF_ERROR(start_matroska_writer_thread(context));

    context->header_written = true;

    return K4A_RESULT_SUCCEEDED;
}

k4a_result_t k4a_record_write_capture(const k4a_record_t recording_handle, k4a_capture_t capture)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);

    if (!context->header_written)
    {
        logger_error(LOGGER_RECORD, "The recording header needs to be written before any captures.");
        return K4A_RESULT_FAILED;
    }

    // Arrays used to map image formats to tracks, these 2 arrays are order dependant.
    k4a_image_t images[] = {
        k4a_capture_get_color_image(capture),
        k4a_capture_get_depth_image(capture),
        k4a_capture_get_ir_image(capture),
    };
    k4a_image_format_t expected_formats[] = { context->color_format, K4A_IMAGE_FORMAT_DEPTH16, K4A_IMAGE_FORMAT_IR16 };
    KaxTrackEntry *tracks[] = { context->color_track, context->depth_track, context->ir_track };
    static_assert(arraysize(images) == arraysize(tracks), "Invalid mapping from images to track");
    static_assert(arraysize(images) == arraysize(expected_formats), "Invalid mapping from images to formats");

    k4a_result_t result = K4A_RESULT_SUCCEEDED;
    for (size_t i = 0; i < arraysize(images); i++)
    {
        if (images[i])
        {
            size_t buffer_size = k4a_image_get_size(images[i]);
            uint8_t *image_buffer = k4a_image_get_buffer(images[i]);
            if (image_buffer != NULL && buffer_size > 0)
            {
                assert(k4a_image_get_format(images[i]) == expected_formats[i]);

                uint64_t timestamp_ns = k4a_image_get_timestamp_usec(images[i]) * 1000;
                assert(buffer_size <= UINT32_MAX);
                // TODO: BUG 19475311 - Frame needs to be copied until color capture bug is fixed.
                DataBuffer *data_buffer = new DataBuffer(image_buffer, (uint32)buffer_size, NULL, true);
                k4a_result_t tmp_result = TRACE_CALL(write_track_data(context, tracks[i], timestamp_ns, data_buffer));
                if (K4A_FAILED(tmp_result))
                {
                    // Write as many of the image buffers as possible, even if some fail due to timestamp.
                    result = tmp_result;
                    data_buffer->FreeBuffer(*data_buffer);
                    delete data_buffer;
                }
            }
            k4a_image_release(images[i]);
        }
    }

    return result;
}

k4a_result_t k4a_record_write_imu_sample(const k4a_record_t recording_handle, k4a_imu_sample_t imu_sample)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);

    if (!context->header_written)
    {
        logger_error(LOGGER_RECORD, "The recording header needs to be written before any imu samples.");
        return K4A_RESULT_FAILED;
    }

    if (context->imu_buffer->empty())
    {
        context->imu_buffer_start_ns = imu_sample.acc_timestamp_usec * 1000;
    }
    else if (context->imu_buffer_start_ns + (1_s / context->camera_fps) < imu_sample.acc_timestamp_usec * 1000)
    {
        // Write imu samples to disk in blocks the same length as a camera frame.
        k4a_result_t result = TRACE_CALL(flush_imu_buffer(context));
        if (K4A_FAILED(result))
        {
            return result;
        }
        context->imu_buffer_start_ns = imu_sample.acc_timestamp_usec * 1000;
    }

    // Sample is stored as [acc_timestamp, acc_data[3], gyro_timestamp, gyro_data[3]]
    uint8_t sample_data[sizeof(float) * 6 + sizeof(uint64_t) * 2];
    static_assert(sizeof(sample_data) == sizeof(imu_sample.acc_sample.v) + sizeof(imu_sample.acc_timestamp_usec) +
                                             sizeof(imu_sample.gyro_timestamp_usec) + sizeof(imu_sample.gyro_sample.v),
                  "Size of IMU data structure has changed from on-disk format.");
    uint8_t *data_ptr = &sample_data[0];
    data_ptr += write_bytes<uint64_t>(data_ptr, imu_sample.acc_timestamp_usec * 1000);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.acc_sample.v[0]);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.acc_sample.v[1]);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.acc_sample.v[2]);
    data_ptr += write_bytes<uint64_t>(data_ptr, imu_sample.gyro_timestamp_usec * 1000);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.gyro_sample.v[0]);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.gyro_sample.v[1]);
    data_ptr += write_bytes<float>(data_ptr, imu_sample.gyro_sample.v[2]);

    context->imu_buffer->insert(context->imu_buffer->end(), std::begin(sample_data), std::end(sample_data));

    return K4A_RESULT_SUCCEEDED;
}

k4a_result_t k4a_record_flush(const k4a_record_t recording_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(K4A_RESULT_FAILED, k4a_record_t, recording_handle);

    k4a_result_t result = K4A_RESULT_SUCCEEDED;
    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, context == NULL);
    RETURN_VALUE_IF_ARG(K4A_RESULT_FAILED, !context->header_written);

    // Lock the writer thread first so we don't have conflicts
    if (Lock(context->writer_lock) == LOCK_OK)
    {
        if (!context->imu_buffer->empty())
        {
            // If this fails, we may still be able to write other pending clusters below.
            result = TRACE_CALL(flush_imu_buffer(context));
        }

        if (Lock(context->pending_cluster_lock) == LOCK_OK)
        {
            if (!context->pending_clusters->empty())
            {
                for (cluster_t *cluster : *context->pending_clusters)
                {
                    k4a_result_t write_result = TRACE_CALL(
                        write_cluster(context, cluster, &context->last_written_timestamp));
                    if (K4A_FAILED(write_result))
                    {
                        // Try to flush as much of the recording as possible to disk before returning any errors.
                        result = write_result;
                    }
                }
                context->pending_clusters->clear();
            }

            try
            {
                auto &segment_info = GetChild<KaxInfo>(*context->file_segment);

                uint64_t current_position = context->ebml_file->getFilePointer();

                // Update segment info
                GetChild<KaxDuration>(segment_info)
                    .SetValue((double)((context->most_recent_timestamp - context->start_timestamp_offset) /
                                       context->timecode_scale));
                context->segment_info_void->ReplaceWith(segment_info, *context->ebml_file);

                // Render cues
                auto &cues = GetChild<KaxCues>(*context->file_segment);
                cues.Render(*context->ebml_file);

                { // Update seek info
                    auto &seek_head = GetChild<KaxSeekHead>(*context->file_segment);
                    seek_head.RemoveAll(); // Remove any seek entries from previous flushes

                    seek_head.IndexThis(segment_info, *context->file_segment);

                    auto &tracks = GetChild<KaxTracks>(*context->file_segment);
                    seek_head.IndexThis(tracks, *context->file_segment);

                    auto &attachments = GetChild<KaxAttachments>(*context->file_segment);
                    seek_head.IndexThis(attachments, *context->file_segment);

                    auto &tags = GetChild<KaxTags>(*context->file_segment);
                    seek_head.IndexThis(tags, *context->file_segment);

                    seek_head.IndexThis(cues, *context->file_segment);

                    context->seek_void->ReplaceWith(seek_head, *context->ebml_file);
                }

                // Update the file segment head to write the current size
                context->ebml_file->setFilePointer(0, seek_end);
                uint64 segment_size = context->ebml_file->getFilePointer() -
                                      context->file_segment->GetElementPosition() - context->file_segment->HeadSize();
                // Segment size can only be set once normally, so force the flag.
                context->file_segment->SetSizeInfinite(true);
                if (!context->file_segment->ForceSize(segment_size))
                {
                    logger_error(LOGGER_RECORD, "Failed set file segment size.");
                }
                context->file_segment->OverwriteHead(*context->ebml_file);

                // Set the write pointer back in case we're not done recording yet.
                assert(current_position <= INT64_MAX);
                context->ebml_file->setFilePointer((int64_t)current_position);
            }
            catch (std::ios_base::failure e)
            {
                logger_error(LOGGER_RECORD, "Failed to write recording '%s': %s", context->file_path, e.what());
                result = K4A_RESULT_FAILED;
            }
            Unlock(context->pending_cluster_lock);
        }
        Unlock(context->writer_lock);
    }
    return result;
}

void k4a_record_close(const k4a_record_t recording_handle)
{
    RETURN_VALUE_IF_HANDLE_INVALID(VOID_VALUE, k4a_record_t, recording_handle);

    k4a_record_context_t *context = k4a_record_t_get_context(recording_handle);
    if (context != NULL)
    {
        // If the recording was started, flush any unwritten data.
        if (context->header_written)
        {
            // If these fail, there's nothing we can do but log.
            (void)TRACE_CALL(k4a_record_flush(recording_handle));
            (void)TRACE_CALL(stop_matroska_writer_thread(context));
        }

        Lock_Deinit(context->pending_cluster_lock);

        try
        {
            context->ebml_file->close();
        }
        catch (std::ios_base::failure e)
        {
            logger_error(LOGGER_RECORD, "Failed to close recording '%s': %s", context->file_path, e.what());
        }

        // After this destroy, logging will no longer happen.
        if (context->logger_handle)
        {
            logger_destroy(context->logger_handle);
        }
    }
    k4a_record_t_destroy(recording_handle);
}