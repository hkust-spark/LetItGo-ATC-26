#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define REPLAY_CACHE_LOOKAHEAD_PACKETS 32

typedef struct {
    const char *input_path;
    const char *output_path;
    int64_t target_timestamp;
    int64_t target_packet_offset;
    int transport_packet_size;
    int corrupt_packet_index;
} Options;

typedef struct {
    AVPacket **packets;
    int count;
    int capacity;
    int target_packet_index;
} PacketReplayCache;

typedef struct {
    int width;
    int height;
    uint8_t *data;
} RgbImage;

typedef struct {
    int width;
    int height;
    int valid_width;
    int valid_height;
    const uint8_t *data;
    double *mean;
    double *variance;
} SsimReferenceWindows;

typedef struct {
    int width;
    int height;
    double *integrals;
} SsimWorkspace;

typedef struct {
    char block_unit[32];
    int block_width;
    int block_height;
    int total_blocks;
    int damaged_blocks;
    int concealed_blocks;
    int inter_concealed_blocks;
    int decode_error_flags;
    int frame_corrupt;
    int recovery_total_mbs;
    int recovery_decoded_ok_mbs;
    int recovery_mv_guessed_mbs;
    int recovery_spatial_concealed_mbs;
    int recovery_reference_copied_mbs;
    int recovery_failed_or_unclassified_mbs;
} BlockDamageMetrics;

typedef struct {
    int packet_index;
    FILE *output;
} MotionVectorDumpContext;

typedef struct {
    int packet_index;
    FILE *output;
    int clean_mode;
} GuessDcDumpContext;

typedef struct {
    int packet_index;
    FILE *output;
    int clean_mode;
} GuessMvDumpContext;

typedef struct {
    int valid;
    int area;
    int motion_x;
    int motion_y;
    int truth_ref;
    int motion_scale;
} GuessMvTruthCell;

static int compute_fast_ssim_rgb_strided(
    const SsimReferenceWindows *reference,
    SsimWorkspace *workspace,
    const uint8_t *candidate_data,
    int candidate_width,
    int candidate_height,
    int candidate_stride,
    double *ssim
);

static void usage(const char *program_name)
{
    fprintf(
        stderr,
        "Usage: %s <input_path> <output_path> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> [corrupt_packet_index]\n"
        "       %s --batch <input_path> <output_dir> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> <start_corrupt_packet_index> <stop_corrupt_packet_index> "
        "[output_extension] [ssim_reference_ppm]\n"
        "       %s --error-blocks <input_path> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> <start_corrupt_packet_index> <stop_corrupt_packet_index>\n"
        "       %s --mv-dump <input_path> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> <start_corrupt_packet_index> <stop_corrupt_packet_index>\n"
        "       %s --guess-dc-dump <input_path> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> <start_corrupt_packet_index> <stop_corrupt_packet_index>\n"
        "       %s --guess-mv-dump <input_path> <target_timestamp> <target_packet_offset> "
        "<transport_packet_size> <start_corrupt_packet_index> <stop_corrupt_packet_index>\n",
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name
    );
}

static void reset_block_damage_metrics(BlockDamageMetrics *metrics)
{
    if (metrics == NULL) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
    snprintf(metrics->block_unit, sizeof(metrics->block_unit), "%s", "unsupported");
    metrics->block_width = -1;
    metrics->block_height = -1;
    metrics->total_blocks = -1;
    metrics->damaged_blocks = -1;
    metrics->concealed_blocks = -1;
    metrics->inter_concealed_blocks = -1;
    metrics->recovery_total_mbs = -1;
    metrics->recovery_decoded_ok_mbs = -1;
    metrics->recovery_mv_guessed_mbs = -1;
    metrics->recovery_spatial_concealed_mbs = -1;
    metrics->recovery_reference_copied_mbs = -1;
    metrics->recovery_failed_or_unclassified_mbs = -1;
}

static int read_frame_metadata_int(const AVFrame *frame, const char *key, int default_value)
{
    const AVDictionaryEntry *entry = av_dict_get(frame->metadata, key, NULL, 0);
    char *end = NULL;
    long parsed = 0;
    if (entry == NULL || entry->value == NULL) {
        return default_value;
    }

    errno = 0;
    parsed = strtol(entry->value, &end, 10);
    if (errno != 0 || end == entry->value || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) {
        return default_value;
    }
    return (int)parsed;
}

static void read_frame_metadata_string(
    const AVFrame *frame,
    const char *key,
    char *buffer,
    size_t buffer_size,
    const char *default_value
)
{
    const AVDictionaryEntry *entry = av_dict_get(frame->metadata, key, NULL, 0);
    const char *value = default_value;
    if (entry != NULL && entry->value != NULL && entry->value[0] != '\0') {
        value = entry->value;
    }
    snprintf(buffer, buffer_size, "%s", value);
}

static void read_block_damage_metrics(const AVFrame *frame, BlockDamageMetrics *metrics)
{
    if (metrics == NULL) {
        return;
    }

    reset_block_damage_metrics(metrics);
    metrics->decode_error_flags = frame->decode_error_flags;
    metrics->frame_corrupt = (frame->flags & AV_FRAME_FLAG_CORRUPT) ? 1 : 0;
    read_frame_metadata_string(
        frame,
        "packet_loss.error_unit",
        metrics->block_unit,
        sizeof(metrics->block_unit),
        "unsupported"
    );
    metrics->block_width = read_frame_metadata_int(frame, "packet_loss.error_unit_width", -1);
    metrics->block_height = read_frame_metadata_int(frame, "packet_loss.error_unit_height", -1);
    metrics->total_blocks = read_frame_metadata_int(frame, "packet_loss.error_total_blocks", -1);
    metrics->damaged_blocks = read_frame_metadata_int(frame, "packet_loss.error_damaged_blocks", -1);
    metrics->concealed_blocks = read_frame_metadata_int(frame, "packet_loss.error_concealed_blocks", -1);
    metrics->inter_concealed_blocks = read_frame_metadata_int(
        frame,
        "packet_loss.error_inter_concealed_blocks",
        -1
    );
    metrics->recovery_total_mbs = read_frame_metadata_int(frame, "packet_loss.recovery_total_mbs", -1);
    metrics->recovery_decoded_ok_mbs = read_frame_metadata_int(
        frame,
        "packet_loss.recovery_decoded_ok_mbs",
        -1
    );
    metrics->recovery_mv_guessed_mbs = read_frame_metadata_int(
        frame,
        "packet_loss.recovery_mv_guessed_mbs",
        -1
    );
    metrics->recovery_spatial_concealed_mbs = read_frame_metadata_int(
        frame,
        "packet_loss.recovery_spatial_concealed_mbs",
        -1
    );
    metrics->recovery_reference_copied_mbs = read_frame_metadata_int(
        frame,
        "packet_loss.recovery_reference_copied_mbs",
        -1
    );
    metrics->recovery_failed_or_unclassified_mbs = read_frame_metadata_int(
        frame,
        "packet_loss.recovery_failed_or_unclassified_mbs",
        -1
    );
}

static void print_block_damage_result(
    int packet_index,
    int success,
    const BlockDamageMetrics *metrics
)
{
    double damaged_ratio = NAN;

    if (success && metrics->damaged_blocks >= 0 && metrics->total_blocks > 0) {
        damaged_ratio = (double)metrics->damaged_blocks / (double)metrics->total_blocks;
    }

    printf(
        "%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.17g\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
        packet_index,
        success,
        metrics->block_unit,
        metrics->block_width,
        metrics->block_height,
        metrics->total_blocks,
        metrics->damaged_blocks,
        metrics->concealed_blocks,
        metrics->inter_concealed_blocks,
        damaged_ratio,
        metrics->decode_error_flags,
        metrics->frame_corrupt,
        metrics->recovery_total_mbs,
        metrics->recovery_decoded_ok_mbs,
        metrics->recovery_mv_guessed_mbs,
        metrics->recovery_spatial_concealed_mbs,
        metrics->recovery_reference_copied_mbs,
        metrics->recovery_failed_or_unclassified_mbs
    );
}

static int floor_div_int(int value, int divisor)
{
    if (divisor <= 0) {
        return 0;
    }
    if (value >= 0) {
        return value / divisor;
    }
    return -(((-value) + divisor - 1) / divisor);
}

static int frame_component_sample_depth(
    const AVPixFmtDescriptor *descriptor,
    int component
)
{
    if (descriptor == NULL || component < 0 || component >= descriptor->nb_components) {
        return 8;
    }
    return descriptor->comp[component].depth;
}

static double frame_component_block_mean(
    const AVFrame *frame,
    const AVPixFmtDescriptor *descriptor,
    int component,
    int luma_x_start,
    int luma_x_stop,
    int luma_y_start,
    int luma_y_stop
)
{
    const AVComponentDescriptor *component_descriptor = NULL;
    int plane = 0;
    int shift_w = 0;
    int shift_h = 0;
    int plane_width = 0;
    int plane_height = 0;
    int x_start = 0;
    int x_stop = 0;
    int y_start = 0;
    int y_stop = 0;
    int sample_depth = 8;
    int sample_step = 1;
    int sample_offset = 0;
    int sample_shift = 0;
    int sample_count = 0;
    double sample_sum = 0.0;

    if (frame == NULL || descriptor == NULL || component < 0 || component >= descriptor->nb_components) {
        return NAN;
    }

    component_descriptor = &descriptor->comp[component];
    plane = component_descriptor->plane;
    if (plane < 0 || plane >= AV_NUM_DATA_POINTERS || frame->data[plane] == NULL || frame->linesize[plane] == 0) {
        return NAN;
    }

    if (component > 0) {
        shift_w = descriptor->log2_chroma_w;
        shift_h = descriptor->log2_chroma_h;
    }
    plane_width = (frame->width + (1 << shift_w) - 1) >> shift_w;
    plane_height = (frame->height + (1 << shift_h) - 1) >> shift_h;
    x_start = luma_x_start >> shift_w;
    x_stop = (luma_x_stop + (1 << shift_w) - 1) >> shift_w;
    y_start = luma_y_start >> shift_h;
    y_stop = (luma_y_stop + (1 << shift_h) - 1) >> shift_h;
    if (x_stop > plane_width) {
        x_stop = plane_width;
    }
    if (y_stop > plane_height) {
        y_stop = plane_height;
    }
    if (x_start < 0 || y_start < 0 || x_stop <= x_start || y_stop <= y_start) {
        return NAN;
    }

    sample_depth = frame_component_sample_depth(descriptor, component);
    sample_step = component_descriptor->step > 0 ? component_descriptor->step : (sample_depth + 7) / 8;
    sample_offset = component_descriptor->offset;
    sample_shift = component_descriptor->shift;
    for (int y = y_start; y < y_stop; y++) {
        const uint8_t *row = frame->data[plane] + (ptrdiff_t)y * frame->linesize[plane];
        for (int x = x_start; x < x_stop; x++) {
            const uint8_t *sample = row + sample_offset + (ptrdiff_t)x * sample_step;
            uint16_t sample_value = 0;
            if (sample_depth <= 8) {
                sample_value = sample[0];
            } else {
                if ((descriptor->flags & AV_PIX_FMT_FLAG_BE) != 0) {
                    sample_value = ((uint16_t)sample[0] << 8) | sample[1];
                } else {
                    sample_value = ((uint16_t)sample[1] << 8) | sample[0];
                }
            }
            if (sample_shift > 0) {
                sample_value >>= sample_shift;
            }
            sample_sum += sample_value;
            sample_count += 1;
        }
    }

    if (sample_count <= 0) {
        return NAN;
    }
    return sample_sum / sample_count;
}

static int dump_guess_dc_truth_from_frame(
    const AVFrame *frame,
    const GuessDcDumpContext *dump_context
)
{
    int frame_width = 0;
    int frame_height = 0;
    int64_t frame_timestamp = AV_NOPTS_VALUE;
    const AVPixFmtDescriptor *descriptor = NULL;

    if (frame == NULL || dump_context == NULL || dump_context->output == NULL) {
        return 0;
    }
    if (frame->data[0] == NULL || frame->linesize[0] == 0 || frame->width <= 0 || frame->height <= 0) {
        fprintf(stderr, "Target frame does not have a readable luma plane\n");
        return AVERROR_INVALIDDATA;
    }

    frame_width = frame->width;
    frame_height = frame->height;
    descriptor = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
    if (descriptor == NULL || descriptor->nb_components <= 0) {
        fprintf(stderr, "Target frame does not have a readable pixel format descriptor\n");
        return AVERROR_INVALIDDATA;
    }
    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }

    for (int component = 0; component < 3; component++) {
        const int block_size = component == 0 ? 8 : 16;
        const int block_width = (frame_width + block_size - 1) / block_size;
        const int block_height = (frame_height + block_size - 1) / block_size;

        if (component >= descriptor->nb_components) {
            continue;
        }

        for (int block_y = 0; block_y < block_height; block_y++) {
            const int y_start = block_y * block_size;
            const int y_stop = y_start + block_size > frame_height ? frame_height : y_start + block_size;
            for (int block_x = 0; block_x < block_width; block_x++) {
                const int x_start = block_x * block_size;
                const int x_stop = x_start + block_size > frame_width ? frame_width : x_start + block_size;
                const double sample_mean = frame_component_block_mean(
                    frame,
                    descriptor,
                    component,
                    x_start,
                    x_stop,
                    y_start,
                    y_stop
                );
                int clean_dc = 0;

                if (isnan(sample_mean)) {
                    continue;
                }
                clean_dc = (int)(sample_mean * 8.0 + 0.5);
                fprintf(
                    dump_context->output,
                    "%d\t%d\t%" PRId64 "\t%d\t%d\t%d\t%d\t%d\t%d\n",
                    dump_context->packet_index,
                    1,
                    frame_timestamp,
                    component,
                    block_x,
                    block_y,
                    component == 0 ? block_x / 2 : block_x,
                    component == 0 ? block_y / 2 : block_y,
                    clean_dc
                );
            }
        }
    }

    return 0;
}

static int dump_guess_dc_records_from_frame(
    const AVFrame *frame,
    const GuessDcDumpContext *dump_context
)
{
    const AVDictionaryEntry *entry = NULL;
    const char *cursor = NULL;
    int64_t frame_timestamp = AV_NOPTS_VALUE;

    if (frame == NULL || dump_context == NULL || dump_context->output == NULL) {
        return 0;
    }
    if (dump_context->clean_mode) {
        return dump_guess_dc_truth_from_frame(frame, dump_context);
    }

    entry = av_dict_get(
        frame->metadata,
        "packet_loss.guess_dc_records",
        NULL,
        0
    );
    if (entry == NULL || entry->value == NULL || entry->value[0] == '\0') {
        return 0;
    }

    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }

    cursor = entry->value;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);
        if (line_length > 0) {
            size_t index = 0;
            fprintf(
                dump_context->output,
                "%d\t%d\t%" PRId64 "\t",
                dump_context->packet_index,
                1,
                frame_timestamp
            );
            for (index = 0; index < line_length; index++) {
                fputc(cursor[index] == ',' ? '\t' : cursor[index], dump_context->output);
            }
            fputc('\n', dump_context->output);
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    return 0;
}

static int dump_motion_vectors_from_frame(
    const AVFrame *frame,
    const MotionVectorDumpContext *dump_context
)
{
    const AVFrameSideData *side_data = NULL;
    const AVMotionVector *motion_vectors = NULL;
    int motion_vector_count = 0;
    int er_origin_mb_x = -1;
    int er_origin_mb_y = -1;
    int er_total_blocks = -1;
    int er_damaged_mb_count = 0;
    int frame_corrupt = 0;
    int64_t frame_timestamp = AV_NOPTS_VALUE;

    if (frame == NULL || dump_context == NULL || dump_context->output == NULL) {
        return 0;
    }

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (side_data == NULL || side_data->data == NULL || side_data->size == 0) {
        return 0;
    }
    if (side_data->size % sizeof(*motion_vectors) != 0) {
        fprintf(stderr, "Invalid motion-vector side data size: %zu\n", side_data->size);
        return AVERROR_INVALIDDATA;
    }

    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }
    er_origin_mb_x = read_frame_metadata_int(frame, "packet_loss.er_origin_mb_x", -1);
    er_origin_mb_y = read_frame_metadata_int(frame, "packet_loss.er_origin_mb_y", -1);
    er_total_blocks = read_frame_metadata_int(frame, "packet_loss.error_total_blocks", -1);
    er_damaged_mb_count = read_frame_metadata_int(frame, "packet_loss.er_damaged_mb_count", 0);
    frame_corrupt = (frame->flags & AV_FRAME_FLAG_CORRUPT) ? 1 : 0;

    motion_vectors = (const AVMotionVector *)side_data->data;
    motion_vector_count = (int)(side_data->size / sizeof(*motion_vectors));
    for (int i = 0; i < motion_vector_count; i++) {
        const AVMotionVector *motion_vector = &motion_vectors[i];
        const int mb_x = floor_div_int(motion_vector->dst_x, 16);
        const int mb_y = floor_div_int(motion_vector->dst_y, 16);
        int manhattan_to_origin = -1;

        if (er_origin_mb_x >= 0 && er_origin_mb_y >= 0) {
            manhattan_to_origin = abs(mb_x - er_origin_mb_x) + abs(mb_y - er_origin_mb_y);
        }

        fprintf(
            dump_context->output,
            "%d\t%d\t%" PRId64 "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%" PRIu64
            "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
            dump_context->packet_index,
            1,
            frame_timestamp,
            motion_vector->source,
            motion_vector->dst_x,
            motion_vector->dst_y,
            mb_x,
            mb_y,
            motion_vector->motion_x,
            motion_vector->motion_y,
            motion_vector->motion_scale,
            motion_vector->src_x,
            motion_vector->src_y,
            motion_vector->source,
            motion_vector->w,
            motion_vector->h,
            motion_vector->flags,
            er_origin_mb_x,
            er_origin_mb_y,
            er_total_blocks,
            er_damaged_mb_count,
            manhattan_to_origin,
            frame->decode_error_flags,
            frame_corrupt
        );
    }

    return 0;
}

static int dump_guess_mv_truth_from_frame(
    const AVFrame *frame,
    const GuessMvDumpContext *dump_context
)
{
    const AVFrameSideData *side_data = NULL;
    const AVMotionVector *motion_vectors = NULL;
    const AVDictionaryEntry *metadata_entry = NULL;
    const char *cursor = NULL;
    GuessMvTruthCell *truth_cells = NULL;
    int motion_vector_count = 0;
    int mb_width = 0;
    int mb_height = 0;
    int cell_count = 0;
    int64_t frame_timestamp = AV_NOPTS_VALUE;

    if (frame == NULL || dump_context == NULL || dump_context->output == NULL) {
        return 0;
    }

    metadata_entry = av_dict_get(
        frame->metadata,
        "packet_loss.guess_mv_truth_records",
        NULL,
        0
    );
    if (metadata_entry != NULL && metadata_entry->value != NULL &&
        metadata_entry->value[0] != '\0') {
        goto metadata_fallback;
    }

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (side_data == NULL || side_data->data == NULL || side_data->size == 0) {
        goto metadata_fallback;
    }
    if (side_data->size % sizeof(*motion_vectors) != 0) {
        fprintf(stderr, "Invalid motion-vector side data size: %zu\n", side_data->size);
        return AVERROR_INVALIDDATA;
    }

    mb_width = (frame->width + 15) / 16;
    mb_height = (frame->height + 15) / 16;
    cell_count = mb_width * mb_height;
    if (cell_count <= 0) {
        return 0;
    }
    truth_cells = calloc((size_t)cell_count, sizeof(*truth_cells));
    if (truth_cells == NULL) {
        return AVERROR(ENOMEM);
    }

    motion_vectors = (const AVMotionVector *)side_data->data;
    motion_vector_count = (int)(side_data->size / sizeof(*motion_vectors));
    for (int i = 0; i < motion_vector_count; i++) {
        const AVMotionVector *motion_vector = &motion_vectors[i];
        const int mb_x = floor_div_int(motion_vector->dst_x, 16);
        const int mb_y = floor_div_int(motion_vector->dst_y, 16);
        const int cell_index = mb_x + mb_y * mb_width;
        const int area = motion_vector->w > 0 && motion_vector->h > 0 ?
                         motion_vector->w * motion_vector->h : 1;
        GuessMvTruthCell *cell = NULL;

        if (mb_x < 0 || mb_y < 0 || mb_x >= mb_width || mb_y >= mb_height) {
            continue;
        }

        cell = &truth_cells[cell_index];
        if (cell->valid && area <= cell->area) {
            continue;
        }
        cell->valid = 1;
        cell->area = area;
        cell->motion_x = motion_vector->motion_x;
        cell->motion_y = motion_vector->motion_y;
        cell->truth_ref = motion_vector->source;
        cell->motion_scale = motion_vector->motion_scale;
    }

    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }

    for (int mb_y = 0; mb_y < mb_height; mb_y++) {
        for (int mb_x = 0; mb_x < mb_width; mb_x++) {
            const GuessMvTruthCell *cell = &truth_cells[mb_x + mb_y * mb_width];
            if (!cell->valid) {
                continue;
            }
            fprintf(
                dump_context->output,
                "%d\t%d\t%" PRId64 "\t%d\t%d\t%d\t%d\t%d\t%d\n",
                dump_context->packet_index,
                1,
                frame_timestamp,
                mb_x,
                mb_y,
                cell->motion_x,
                cell->motion_y,
                cell->truth_ref,
                cell->motion_scale
            );
        }
    }

    free(truth_cells);
    return 0;

metadata_fallback:
    metadata_entry = av_dict_get(
        frame->metadata,
        "packet_loss.guess_mv_truth_records",
        NULL,
        0
    );
    if (metadata_entry == NULL || metadata_entry->value == NULL ||
        metadata_entry->value[0] == '\0') {
        return 0;
    }

    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }

    cursor = metadata_entry->value;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);
        if (line_length > 0) {
            size_t index = 0;
            fprintf(
                dump_context->output,
                "%d\t%d\t%" PRId64 "\t",
                dump_context->packet_index,
                1,
                frame_timestamp
            );
            for (index = 0; index < line_length; index++) {
                fputc(cursor[index] == ',' ? '\t' : cursor[index], dump_context->output);
            }
            fputc('\n', dump_context->output);
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }
    return 0;
}

static int dump_guess_mv_records_from_frame(
    const AVFrame *frame,
    const GuessMvDumpContext *dump_context
)
{
    const AVDictionaryEntry *entry = NULL;
    const char *cursor = NULL;
    int64_t frame_timestamp = AV_NOPTS_VALUE;

    if (frame == NULL || dump_context == NULL || dump_context->output == NULL) {
        return 0;
    }
    if (dump_context->clean_mode) {
        return dump_guess_mv_truth_from_frame(frame, dump_context);
    }

    entry = av_dict_get(
        frame->metadata,
        "packet_loss.guess_mv_records",
        NULL,
        0
    );
    if (entry == NULL || entry->value == NULL || entry->value[0] == '\0') {
        return 0;
    }

    frame_timestamp = frame->best_effort_timestamp;
    if (frame_timestamp == AV_NOPTS_VALUE) {
        frame_timestamp = frame->pts;
    }

    cursor = entry->value;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);
        if (line_length > 0) {
            size_t index = 0;
            fprintf(
                dump_context->output,
                "%d\t%d\t%" PRId64 "\t",
                dump_context->packet_index,
                1,
                frame_timestamp
            );
            for (index = 0; index < line_length; index++) {
                fputc(cursor[index] == ',' ? '\t' : cursor[index], dump_context->output);
            }
            fputc('\n', dump_context->output);
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    return 0;
}

static int parse_int64_arg(const char *value, const char *name, int64_t *parsed)
{
    char *end = NULL;
    errno = 0;
    *parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid %s: %s\n", name, value);
        return -1;
    }
    return 0;
}

static int parse_int_arg(const char *value, const char *name, int *parsed)
{
    int64_t temporary = 0;
    if (parse_int64_arg(value, name, &temporary) < 0) {
        return -1;
    }
    if (temporary < INT32_MIN || temporary > INT32_MAX) {
        fprintf(stderr, "%s is out of range: %s\n", name, value);
        return -1;
    }
    *parsed = (int)temporary;
    return 0;
}

static int open_video_decoder(
    AVFormatContext *format_context,
    int *video_stream_index,
    AVCodecContext **decoder_context,
    int export_motion_vectors
)
{
    const AVCodec *decoder = NULL;
    AVStream *stream = NULL;
    int ret = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find a video stream (%s)\n", av_err2str(ret));
        return ret;
    }

    *video_stream_index = ret;
    stream = format_context->streams[*video_stream_index];
    if (stream->codecpar->codec_id == AV_CODEC_ID_AV1) {
        decoder = avcodec_find_decoder_by_name("libaom-av1");
    }
    if (decoder == NULL) {
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    }
    if (decoder == NULL) {
        fprintf(stderr, "Could not find a decoder for codec id %d\n", stream->codecpar->codec_id);
        return AVERROR_DECODER_NOT_FOUND;
    }

    *decoder_context = avcodec_alloc_context3(decoder);
    if (*decoder_context == NULL) {
        fprintf(stderr, "Could not allocate decoder context\n");
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(*decoder_context, stream->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Could not copy codec parameters to decoder context (%s)\n", av_err2str(ret));
        return ret;
    }

    (*decoder_context)->pkt_timebase = stream->time_base;
    if (export_motion_vectors) {
        (*decoder_context)->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
    }
    ret = avcodec_open2(*decoder_context, decoder, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open decoder (%s)\n", av_err2str(ret));
        return ret;
    }

    return 0;
}

static int save_frame_as_png(const AVFrame *frame, const char *output_path)
{
    const AVCodec *encoder = NULL;
    AVCodecContext *encoder_context = NULL;
    AVFrame *rgb_frame = NULL;
    AVPacket *packet = NULL;
    struct SwsContext *sws_context = NULL;
    FILE *output_file = NULL;
    int ret = 0;

    encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == NULL) {
        fprintf(stderr, "Could not find PNG encoder\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    encoder_context = avcodec_alloc_context3(encoder);
    if (encoder_context == NULL) {
        fprintf(stderr, "Could not allocate PNG encoder context\n");
        return AVERROR(ENOMEM);
    }

    encoder_context->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder_context->codec_id = AV_CODEC_ID_PNG;
    encoder_context->pix_fmt = AV_PIX_FMT_RGB24;
    encoder_context->width = frame->width;
    encoder_context->height = frame->height;
    encoder_context->time_base = (AVRational){1, 25};

    ret = avcodec_open2(encoder_context, encoder, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open PNG encoder (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    rgb_frame = av_frame_alloc();
    if (rgb_frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    rgb_frame->format = encoder_context->pix_fmt;
    rgb_frame->width = encoder_context->width;
    rgb_frame->height = encoder_context->height;

    ret = av_frame_get_buffer(rgb_frame, 1);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate RGB frame buffer (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = av_frame_make_writable(rgb_frame);
    if (ret < 0) {
        fprintf(stderr, "Could not make RGB frame writable (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    sws_context = sws_getContext(
        frame->width,
        frame->height,
        (enum AVPixelFormat)frame->format,
        frame->width,
        frame->height,
        encoder_context->pix_fmt,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );
    if (sws_context == NULL) {
        fprintf(stderr, "Could not create swscale context\n");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }

    sws_scale(
        sws_context,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        rgb_frame->data,
        rgb_frame->linesize
    );
    rgb_frame->pts = 0;

    packet = av_packet_alloc();
    if (packet == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    output_file = fopen(output_path, "wb");
    if (output_file == NULL) {
        fprintf(stderr, "Could not open output file %s\n", output_path);
        ret = AVERROR(errno);
        goto cleanup;
    }

    ret = avcodec_send_frame(encoder_context, rgb_frame);
    if (ret < 0) {
        fprintf(stderr, "Could not submit RGB frame to PNG encoder (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    while (1) {
        ret = avcodec_receive_packet(encoder_context, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            fprintf(stderr, "Could not receive PNG packet (%s)\n", av_err2str(ret));
            goto cleanup;
        }
        if (fwrite(packet->data, 1, packet->size, output_file) != (size_t)packet->size) {
            fprintf(stderr, "Could not write PNG bytes to %s\n", output_path);
            ret = AVERROR(errno);
            av_packet_unref(packet);
            goto cleanup;
        }
        av_packet_unref(packet);
    }

cleanup:
    if (output_file != NULL) {
        fclose(output_file);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (rgb_frame != NULL) {
        av_frame_free(&rgb_frame);
    }
    if (sws_context != NULL) {
        sws_freeContext(sws_context);
    }
    if (encoder_context != NULL) {
        avcodec_free_context(&encoder_context);
    }
    return ret;
}

static int output_path_has_extension(const char *output_path, const char *extension)
{
    size_t output_path_length = strlen(output_path);
    size_t extension_length = strlen(extension);
    if (output_path_length < extension_length) {
        return 0;
    }
    return strcmp(output_path + output_path_length - extension_length, extension) == 0;
}

static int save_frame_as_ppm(
    const AVFrame *frame,
    const char *output_path,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim
)
{
    AVFrame *rgb_frame = NULL;
    struct SwsContext *sws_context = NULL;
    FILE *output_file = NULL;
    int bytes_per_row = frame->width * 3;
    int row_index = 0;
    int ret = 0;

    rgb_frame = av_frame_alloc();
    if (rgb_frame == NULL) {
        return AVERROR(ENOMEM);
    }
    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = frame->width;
    rgb_frame->height = frame->height;

    ret = av_frame_get_buffer(rgb_frame, 1);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate RGB frame buffer (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = av_frame_make_writable(rgb_frame);
    if (ret < 0) {
        fprintf(stderr, "Could not make RGB frame writable (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    sws_context = sws_getContext(
        frame->width,
        frame->height,
        (enum AVPixelFormat)frame->format,
        frame->width,
        frame->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );
    if (sws_context == NULL) {
        fprintf(stderr, "Could not create swscale context\n");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }

    sws_scale(
        sws_context,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        rgb_frame->data,
        rgb_frame->linesize
    );

    if (reference_windows != NULL && ssim_workspace != NULL && ssim != NULL) {
        ret = compute_fast_ssim_rgb_strided(
            reference_windows,
            ssim_workspace,
            rgb_frame->data[0],
            frame->width,
            frame->height,
            rgb_frame->linesize[0],
            ssim
        );
        if (ret < 0) {
            goto cleanup;
        }
    }

    output_file = fopen(output_path, "wb");
    if (output_file == NULL) {
        fprintf(stderr, "Could not open output file %s\n", output_path);
        ret = AVERROR(errno);
        goto cleanup;
    }

    if (fprintf(output_file, "P6\n%d %d\n255\n", frame->width, frame->height) < 0) {
        fprintf(stderr, "Could not write PPM header to %s\n", output_path);
        ret = AVERROR(errno);
        goto cleanup;
    }
    for (row_index = 0; row_index < frame->height; row_index++) {
        uint8_t *row = rgb_frame->data[0] + row_index * rgb_frame->linesize[0];
        if (fwrite(row, 1, (size_t)bytes_per_row, output_file) != (size_t)bytes_per_row) {
            fprintf(stderr, "Could not write PPM bytes to %s\n", output_path);
            ret = AVERROR(errno);
            goto cleanup;
        }
    }

cleanup:
    if (output_file != NULL) {
        fclose(output_file);
    }
    if (rgb_frame != NULL) {
        av_frame_free(&rgb_frame);
    }
    if (sws_context != NULL) {
        sws_freeContext(sws_context);
    }
    return ret;
}

static int save_frame(
    const AVFrame *frame,
    const char *output_path,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim
)
{
    if (output_path_has_extension(output_path, ".ppm")) {
        return save_frame_as_ppm(frame, output_path, reference_windows, ssim_workspace, ssim);
    }
    return save_frame_as_png(frame, output_path);
}

static void free_rgb_image(RgbImage *image)
{
    if (image->data != NULL) {
        free(image->data);
    }
    image->data = NULL;
    image->width = 0;
    image->height = 0;
}

static int read_ppm_token(FILE *input_file, char *buffer, size_t buffer_size)
{
    int value = 0;
    size_t length = 0;

    do {
        value = fgetc(input_file);
        if (value == '#') {
            do {
                value = fgetc(input_file);
            } while (value != EOF && value != '\n' && value != '\r');
        }
    } while (value != EOF && (value == ' ' || value == '\t' || value == '\n' || value == '\r'));

    if (value == EOF) {
        return -1;
    }

    while (value != EOF && value != ' ' && value != '\t' && value != '\n' && value != '\r') {
        if (length + 1 >= buffer_size) {
            return -1;
        }
        buffer[length++] = (char)value;
        value = fgetc(input_file);
    }
    buffer[length] = '\0';
    return 0;
}

static int load_ppm_image(const char *input_path, RgbImage *image)
{
    FILE *input_file = NULL;
    char token[64] = {0};
    int max_value = 0;
    size_t payload_size = 0;
    int ret = 0;

    memset(image, 0, sizeof(*image));
    input_file = fopen(input_path, "rb");
    if (input_file == NULL) {
        fprintf(stderr, "Could not open PPM file %s\n", input_path);
        return AVERROR(errno);
    }

    if (read_ppm_token(input_file, token, sizeof(token)) < 0 || strcmp(token, "P6") != 0) {
        fprintf(stderr, "Unsupported PPM header in %s\n", input_path);
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    if (read_ppm_token(input_file, token, sizeof(token)) < 0) {
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    image->width = atoi(token);
    if (read_ppm_token(input_file, token, sizeof(token)) < 0) {
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    image->height = atoi(token);
    if (read_ppm_token(input_file, token, sizeof(token)) < 0) {
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    max_value = atoi(token);
    if (image->width <= 0 || image->height <= 0 || max_value != 255) {
        fprintf(stderr, "Unsupported PPM dimensions or max value in %s\n", input_path);
        ret = AVERROR(EINVAL);
        goto cleanup;
    }

    payload_size = (size_t)image->width * (size_t)image->height * 3;
    image->data = malloc(payload_size);
    if (image->data == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    if (fread(image->data, 1, payload_size, input_file) != payload_size) {
        fprintf(stderr, "Could not read PPM payload from %s\n", input_path);
        ret = AVERROR(EINVAL);
        goto cleanup;
    }

cleanup:
    if (input_file != NULL) {
        fclose(input_file);
    }
    if (ret < 0) {
        free_rgb_image(image);
    }
    return ret;
}

static double integral_rectangle_sum(
    const double *integral,
    int stride,
    int x0,
    int y0,
    int x1,
    int y1
)
{
    return (
        integral[y1 * stride + x1]
        - integral[y0 * stride + x1]
        - integral[y1 * stride + x0]
        + integral[y0 * stride + x0]
    );
}

static void free_ssim_reference_windows(SsimReferenceWindows *reference)
{
    if (reference->mean != NULL) {
        free(reference->mean);
    }
    if (reference->variance != NULL) {
        free(reference->variance);
    }
    memset(reference, 0, sizeof(*reference));
}

static void free_ssim_workspace(SsimWorkspace *workspace)
{
    if (workspace->integrals != NULL) {
        free(workspace->integrals);
    }
    memset(workspace, 0, sizeof(*workspace));
}

static int init_ssim_reference_windows(const RgbImage *image, SsimReferenceWindows *reference)
{
    const int window_size = 7;
    const int radius = 3;
    const double pixels_per_window = (double)(window_size * window_size);
    const double covariance_normalization = pixels_per_window / (pixels_per_window - 1.0);
    int width = image->width;
    int height = image->height;
    int stride = width + 1;
    int valid_width = width - 2 * radius;
    int valid_height = height - 2 * radius;
    size_t integral_count = (size_t)(width + 1) * (size_t)(height + 1);
    size_t valid_count = (size_t)valid_width * (size_t)valid_height;
    double *integrals = NULL;
    int channel = 0;

    memset(reference, 0, sizeof(*reference));
    if (image->data == NULL || width < window_size || height < window_size) {
        fprintf(stderr, "Reference image is not valid for 7x7 SSIM\n");
        return AVERROR(EINVAL);
    }

    reference->mean = malloc(sizeof(double) * valid_count * 3);
    reference->variance = malloc(sizeof(double) * valid_count * 3);
    integrals = malloc(sizeof(double) * integral_count * 2);
    if (reference->mean == NULL || reference->variance == NULL || integrals == NULL) {
        free(integrals);
        free_ssim_reference_windows(reference);
        return AVERROR(ENOMEM);
    }

    reference->width = width;
    reference->height = height;
    reference->valid_width = valid_width;
    reference->valid_height = valid_height;
    reference->data = image->data;

    for (channel = 0; channel < 3; channel++) {
        double *sum_x = integrals;
        double *sum_xx = sum_x + integral_count;
        int x = 0;
        int y = 0;

        for (x = 0; x < stride; x++) {
            sum_x[x] = 0.0;
            sum_xx[x] = 0.0;
        }

        for (y = 1; y <= height; y++) {
            double row_x = 0.0;
            double row_xx = 0.0;
            int reference_row_offset = (y - 1) * width * 3;
            int integral_row = y * stride;
            int previous_integral_row = (y - 1) * stride;

            sum_x[integral_row] = 0.0;
            sum_xx[integral_row] = 0.0;
            for (x = 1; x <= width; x++) {
                int reference_pixel_offset = reference_row_offset + (x - 1) * 3 + channel;
                double reference_value = (double)image->data[reference_pixel_offset];
                int integral_offset = integral_row + x;
                int previous_integral_offset = previous_integral_row + x;

                row_x += reference_value;
                row_xx += reference_value * reference_value;
                sum_x[integral_offset] = sum_x[previous_integral_offset] + row_x;
                sum_xx[integral_offset] = sum_xx[previous_integral_offset] + row_xx;
            }
        }

        for (y = radius; y < height - radius; y++) {
            for (x = radius; x < width - radius; x++) {
                int x0 = x - radius;
                int y0 = y - radius;
                int x1 = x + radius + 1;
                int y1 = y + radius + 1;
                size_t valid_index = (size_t)(channel * valid_width * valid_height)
                    + (size_t)(y - radius) * (size_t)valid_width
                    + (size_t)(x - radius);
                double mean = integral_rectangle_sum(sum_x, stride, x0, y0, x1, y1) / pixels_per_window;
                double mean_square = integral_rectangle_sum(sum_xx, stride, x0, y0, x1, y1) / pixels_per_window;

                reference->mean[valid_index] = mean;
                reference->variance[valid_index] = covariance_normalization * (mean_square - mean * mean);
            }
        }
    }

    free(integrals);
    return 0;
}

static int init_ssim_workspace(int width, int height, SsimWorkspace *workspace)
{
    size_t integral_count = (size_t)(width + 1) * (size_t)(height + 1);

    memset(workspace, 0, sizeof(*workspace));
    workspace->integrals = malloc(sizeof(double) * integral_count * 3);
    if (workspace->integrals == NULL) {
        return AVERROR(ENOMEM);
    }
    workspace->width = width;
    workspace->height = height;
    return 0;
}

static int compute_fast_ssim_rgb_strided(
    const SsimReferenceWindows *reference,
    SsimWorkspace *workspace,
    const uint8_t *candidate_data,
    int candidate_width,
    int candidate_height,
    int candidate_stride,
    double *ssim
)
{
    const int window_size = 7;
    const int radius = 3;
    const double pixels_per_window = (double)(window_size * window_size);
    const double covariance_normalization = pixels_per_window / (pixels_per_window - 1.0);
    const double c1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double c2 = (0.03 * 255.0) * (0.03 * 255.0);
    int width = reference->width;
    int height = reference->height;
    int stride = width + 1;
    size_t integral_count = (size_t)(width + 1) * (size_t)(height + 1);
    size_t valid_count = (size_t)reference->valid_width * (size_t)reference->valid_height;
    double total_ssim = 0.0;
    int channel = 0;

    if (
        reference->width != candidate_width
        || reference->height != candidate_height
        || reference->data == NULL
        || reference->mean == NULL
        || reference->variance == NULL
        || candidate_data == NULL
        || workspace->width != candidate_width
        || workspace->height != candidate_height
        || workspace->integrals == NULL
    ) {
        fprintf(stderr, "RGB image shapes do not match for SSIM\n");
        return AVERROR(EINVAL);
    }
    if (width < window_size || height < window_size) {
        fprintf(stderr, "Image is too small for 7x7 SSIM\n");
        return AVERROR(EINVAL);
    }

    for (channel = 0; channel < 3; channel++) {
        double *sum_y = workspace->integrals;
        double *sum_yy = sum_y + integral_count;
        double *sum_xy = sum_yy + integral_count;
        double channel_ssim = 0.0;
        int x = 0;
        int y = 0;

        for (x = 0; x < stride; x++) {
            sum_y[x] = 0.0;
            sum_yy[x] = 0.0;
            sum_xy[x] = 0.0;
        }

        for (y = 1; y <= height; y++) {
            double row_y = 0.0;
            double row_yy = 0.0;
            double row_xy = 0.0;
            int reference_row_offset = (y - 1) * width * 3;
            int candidate_row_offset = (y - 1) * candidate_stride;
            int integral_row = y * stride;
            int previous_integral_row = (y - 1) * stride;

            sum_y[integral_row] = 0.0;
            sum_yy[integral_row] = 0.0;
            sum_xy[integral_row] = 0.0;
            for (x = 1; x <= width; x++) {
                int reference_pixel_offset = reference_row_offset + (x - 1) * 3 + channel;
                int candidate_pixel_offset = candidate_row_offset + (x - 1) * 3 + channel;
                double reference_value = (double)reference->data[reference_pixel_offset];
                double candidate_value = (double)candidate_data[candidate_pixel_offset];
                int integral_offset = integral_row + x;
                int previous_integral_offset = previous_integral_row + x;

                row_y += candidate_value;
                row_yy += candidate_value * candidate_value;
                row_xy += reference_value * candidate_value;

                sum_y[integral_offset] = sum_y[previous_integral_offset] + row_y;
                sum_yy[integral_offset] = sum_yy[previous_integral_offset] + row_yy;
                sum_xy[integral_offset] = sum_xy[previous_integral_offset] + row_xy;
            }
        }

        for (y = radius; y < height - radius; y++) {
            for (x = radius; x < width - radius; x++) {
                int x0 = x - radius;
                int y0 = y - radius;
                int x1 = x + radius + 1;
                int y1 = y + radius + 1;
                size_t valid_index = (size_t)(channel * reference->valid_width * reference->valid_height)
                    + (size_t)(y - radius) * (size_t)reference->valid_width
                    + (size_t)(x - radius);
                double ux = reference->mean[valid_index];
                double vx = reference->variance[valid_index];
                double uy = integral_rectangle_sum(sum_y, stride, x0, y0, x1, y1) / pixels_per_window;
                double uyy = integral_rectangle_sum(sum_yy, stride, x0, y0, x1, y1) / pixels_per_window;
                double uxy = integral_rectangle_sum(sum_xy, stride, x0, y0, x1, y1) / pixels_per_window;
                double vy = covariance_normalization * (uyy - uy * uy);
                double vxy = covariance_normalization * (uxy - ux * uy);
                double numerator = (2.0 * ux * uy + c1) * (2.0 * vxy + c2);
                double denominator = (ux * ux + uy * uy + c1) * (vx + vy + c2);
                channel_ssim += numerator / denominator;
            }
        }
        total_ssim += channel_ssim / (double)valid_count;
    }

    *ssim = total_ssim / 3.0;
    return 0;
}

static void clear_packet_loss_corruption_env(void)
{
    unsetenv("PACKET_LOSS_CORRUPT_PACKET_POS");
    unsetenv("PACKET_LOSS_CORRUPT_REL_START");
    unsetenv("PACKET_LOSS_CORRUPT_REL_END");
}

static int set_packet_loss_corruption_env(int64_t packet_pos, int zero_start, int zero_end)
{
    char packet_pos_buffer[64];
    char zero_start_buffer[64];
    char zero_end_buffer[64];

    if (packet_pos < 0) {
        clear_packet_loss_corruption_env();
        return 0;
    }

    snprintf(packet_pos_buffer, sizeof(packet_pos_buffer), "%" PRId64, packet_pos);
    snprintf(zero_start_buffer, sizeof(zero_start_buffer), "%d", zero_start);
    snprintf(zero_end_buffer, sizeof(zero_end_buffer), "%d", zero_end);

    if (setenv("PACKET_LOSS_CORRUPT_PACKET_POS", packet_pos_buffer, 1) < 0 ||
        setenv("PACKET_LOSS_CORRUPT_REL_START", zero_start_buffer, 1) < 0 ||
        setenv("PACKET_LOSS_CORRUPT_REL_END", zero_end_buffer, 1) < 0) {
        fprintf(stderr, "Could not set packet-loss corruption environment\n");
        return AVERROR(errno);
    }
    return 0;
}

static int corrupt_packet_payload(AVPacket *packet, int transport_packet_size, int corrupt_packet_index)
{
    int total_packets = 0;
    int zero_start = 0;
    int zero_end = 0;
    int ret = 0;

    if (corrupt_packet_index == 0) {
        clear_packet_loss_corruption_env();
        return 0;
    }
    if (corrupt_packet_index < 2) {
        clear_packet_loss_corruption_env();
        fprintf(stderr, "corrupt_packet_index must be 0 for clean mode or at least 2 for packet loss\n");
        return AVERROR(EINVAL);
    }
    if (packet->data == NULL) {
        clear_packet_loss_corruption_env();
        fprintf(stderr, "Target packet has no writable payload buffer\n");
        return AVERROR(EINVAL);
    }

    ret = av_packet_make_writable(packet);
    if (ret < 0) {
        clear_packet_loss_corruption_env();
        fprintf(stderr, "Target packet payload is not writable (%s)\n", av_err2str(ret));
        return ret;
    }

    total_packets = (packet->size + transport_packet_size - 1) / transport_packet_size;
    if (corrupt_packet_index > total_packets) {
        clear_packet_loss_corruption_env();
        fprintf(
            stderr,
            "corrupt_packet_index=%d exceeds packet count %d for target frame payload\n",
            corrupt_packet_index,
            total_packets
        );
        return AVERROR(EINVAL);
    }

    zero_start = (corrupt_packet_index - 1) * transport_packet_size;
    zero_end = corrupt_packet_index * transport_packet_size;
    if (zero_end > packet->size) {
        zero_end = packet->size;
    }

    ret = set_packet_loss_corruption_env(packet->pos, zero_start, zero_end);
    if (ret < 0) {
        return ret;
    }

    memset(packet->data + zero_start, 0, (size_t)(zero_end - zero_start));
    return 0;
}

static void free_packet_replay_cache(PacketReplayCache *cache)
{
    int packet_index = 0;
    if (cache == NULL) {
        return;
    }
    for (packet_index = 0; packet_index < cache->count; packet_index++) {
        if (cache->packets[packet_index] != NULL) {
            av_packet_free(&cache->packets[packet_index]);
        }
    }
    free(cache->packets);
    cache->packets = NULL;
    cache->count = 0;
    cache->capacity = 0;
    cache->target_packet_index = -1;
}

static int append_packet_to_replay_cache(PacketReplayCache *cache, const AVPacket *packet)
{
    AVPacket **resized_packets = NULL;
    AVPacket *packet_copy = NULL;
    int new_capacity = 0;

    if (cache->count == cache->capacity) {
        new_capacity = cache->capacity == 0 ? 64 : cache->capacity * 2;
        resized_packets = realloc(cache->packets, (size_t)new_capacity * sizeof(*cache->packets));
        if (resized_packets == NULL) {
            return AVERROR(ENOMEM);
        }
        cache->packets = resized_packets;
        cache->capacity = new_capacity;
    }

    packet_copy = av_packet_clone(packet);
    if (packet_copy == NULL) {
        return AVERROR(ENOMEM);
    }

    cache->packets[cache->count] = packet_copy;
    cache->count += 1;
    return 0;
}

static int drain_decoder_until_target(
    AVCodecContext *decoder_context,
    AVFrame *frame,
    int64_t target_timestamp,
    int can_write_frame,
    const char *output_path,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim,
    BlockDamageMetrics *block_metrics,
    MotionVectorDumpContext *motion_vector_dump,
    GuessDcDumpContext *guess_dc_dump,
    GuessMvDumpContext *guess_mv_dump,
    int *output_written
)
{
    int ret = 0;

    while (1) {
        int64_t frame_timestamp = AV_NOPTS_VALUE;

        ret = avcodec_receive_frame(decoder_context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            fprintf(stderr, "Error while receiving a decoded frame (%s)\n", av_err2str(ret));
            return ret;
        }

        frame_timestamp = frame->best_effort_timestamp;
        if (frame_timestamp == AV_NOPTS_VALUE) {
            frame_timestamp = frame->pts;
        }

        if (can_write_frame && frame_timestamp == target_timestamp) {
            read_block_damage_metrics(frame, block_metrics);
            ret = dump_motion_vectors_from_frame(frame, motion_vector_dump);
            if (ret < 0) {
                av_frame_unref(frame);
                return ret;
            }
            ret = dump_guess_dc_records_from_frame(frame, guess_dc_dump);
            if (ret < 0) {
                av_frame_unref(frame);
                return ret;
            }
            ret = dump_guess_mv_records_from_frame(frame, guess_mv_dump);
            if (ret < 0) {
                av_frame_unref(frame);
                return ret;
            }
            if (output_path != NULL) {
                ret = save_frame(frame, output_path, reference_windows, ssim_workspace, ssim);
                if (ret < 0) {
                    fprintf(stderr, "Could not save the target frame (%s)\n", av_err2str(ret));
                    av_frame_unref(frame);
                    return ret;
                }
            }
            *output_written = 1;
            av_frame_unref(frame);
            return 0;
        }

        av_frame_unref(frame);
    }
}

static int decode_target_frame_attempt(
    AVFormatContext *format_context,
    AVCodecContext *decoder_context,
    AVPacket *packet,
    AVFrame *frame,
    int video_stream_index,
    const Options *options,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim,
    BlockDamageMetrics *block_metrics,
    int64_t seek_timestamp,
    int *target_packet_found,
    int *output_written
)
{
    int ret = 0;

    *target_packet_found = 0;
    *output_written = 0;
    av_packet_unref(packet);
    av_frame_unref(frame);

    ret = av_seek_frame(format_context, video_stream_index, seek_timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(
            stderr,
            "Could not seek to timestamp %" PRId64 " before extracting target frame (%s)\n",
            seek_timestamp,
            av_err2str(ret)
        );
        return ret;
    }
    avcodec_flush_buffers(decoder_context);

    while (!*output_written && av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index != video_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        if (!*target_packet_found && packet->pos == options->target_packet_offset) {
            *target_packet_found = 1;
            ret = corrupt_packet_payload(packet, options->transport_packet_size, options->corrupt_packet_index);
            if (ret < 0) {
                av_packet_unref(packet);
                return ret;
            }
        }

        ret = avcodec_send_packet(decoder_context, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            fprintf(stderr, "Error while submitting a packet for decoding (%s)\n", av_err2str(ret));
            return ret;
        }

        ret = drain_decoder_until_target(
            decoder_context,
            frame,
            options->target_timestamp,
            *target_packet_found,
            options->output_path,
            reference_windows,
            ssim_workspace,
            ssim,
            block_metrics,
            NULL,
            NULL,
            NULL,
            output_written
        );
        if (ret < 0 || *output_written) {
            return ret;
        }
    }

    ret = avcodec_send_packet(decoder_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error while flushing the decoder (%s)\n", av_err2str(ret));
        return ret;
    }

    ret = drain_decoder_until_target(
        decoder_context,
        frame,
        options->target_timestamp,
        *target_packet_found,
        options->output_path,
        reference_windows,
        ssim_workspace,
        ssim,
        block_metrics,
        NULL,
        NULL,
        NULL,
        output_written
    );
    return ret;
}

static int decode_target_frame(
    AVFormatContext *format_context,
    AVCodecContext *decoder_context,
    AVPacket *packet,
    AVFrame *frame,
    int video_stream_index,
    const Options *options,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim,
    BlockDamageMetrics *block_metrics
)
{
    int target_packet_found = 0;
    int output_written = 0;
    int final_target_packet_found = 0;
    int final_output_written = 0;
    int attempt = 0;
    int attempt_count = options->target_timestamp == 0 ? 1 : 2;
    int ret = 0;

    for (attempt = 0; attempt < attempt_count; attempt++) {
        int64_t seek_timestamp = attempt == 0 ? options->target_timestamp : 0;

        if (attempt > 0) {
            fprintf(
                stderr,
                "Retrying target frame extraction from stream start for timestamp %" PRId64 "\n",
                options->target_timestamp
            );
        }

        if (options->output_path != NULL) {
            remove(options->output_path);
        }
        ret = decode_target_frame_attempt(
            format_context,
            decoder_context,
            packet,
            frame,
            video_stream_index,
            options,
            reference_windows,
            ssim_workspace,
            ssim,
            block_metrics,
            seek_timestamp,
            &target_packet_found,
            &output_written
        );

        final_target_packet_found = target_packet_found;
        final_output_written = output_written;
        if (ret >= 0 && target_packet_found && output_written) {
            return 0;
        }
    }

    if (ret < 0) {
        goto done;
    }

    if (!final_target_packet_found) {
        fprintf(
            stderr,
            "Could not find a packet at byte offset %" PRId64 " in %s\n",
            options->target_packet_offset,
            options->input_path
        );
        ret = AVERROR(EINVAL);
        goto done;
    }

    if (!final_output_written) {
        fprintf(
            stderr,
            "Could not decode the target frame at timestamp %" PRId64 "\n",
            options->target_timestamp
        );
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    ret = 0;

done:
    av_packet_unref(packet);
    av_frame_unref(frame);
    return ret < 0 ? ret : 0;
}

static int build_packet_replay_cache_attempt(
    AVFormatContext *format_context,
    AVCodecContext *decoder_context,
    AVPacket *packet,
    AVFrame *frame,
    int video_stream_index,
    const Options *options,
    PacketReplayCache *cache,
    int64_t seek_timestamp,
    int *target_packet_found,
    int *target_frame_seen
)
{
    int lookahead_packets = 0;
    int output_written = 0;
    int ret = 0;

    free_packet_replay_cache(cache);
    cache->target_packet_index = -1;
    *target_packet_found = 0;
    *target_frame_seen = 0;
    av_packet_unref(packet);
    av_frame_unref(frame);

    ret = av_seek_frame(format_context, video_stream_index, seek_timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(
            stderr,
            "Could not seek to timestamp %" PRId64 " before building replay cache (%s)\n",
            seek_timestamp,
            av_err2str(ret)
        );
        return ret;
    }
    avcodec_flush_buffers(decoder_context);

    while (av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index != video_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        ret = append_packet_to_replay_cache(cache, packet);
        if (ret < 0) {
            av_packet_unref(packet);
            return ret;
        }

        if (!*target_packet_found && packet->pos == options->target_packet_offset) {
            *target_packet_found = 1;
            cache->target_packet_index = cache->count - 1;
        }

        if (*target_frame_seen) {
            lookahead_packets += 1;
            av_packet_unref(packet);
            if (lookahead_packets >= REPLAY_CACHE_LOOKAHEAD_PACKETS) {
                break;
            }
            continue;
        }

        ret = avcodec_send_packet(decoder_context, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            fprintf(stderr, "Error while caching packet for replay (%s)\n", av_err2str(ret));
            return ret;
        }

        output_written = 0;
        ret = drain_decoder_until_target(
            decoder_context,
            frame,
            options->target_timestamp,
            *target_packet_found,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            &output_written
        );
        if (ret < 0) {
            return ret;
        }
        if (output_written) {
            *target_frame_seen = 1;
        }
    }

    if (!*target_frame_seen) {
        ret = avcodec_send_packet(decoder_context, NULL);
        if (ret < 0) {
            fprintf(stderr, "Error while flushing replay-cache decoder (%s)\n", av_err2str(ret));
            return ret;
        }

        output_written = 0;
        ret = drain_decoder_until_target(
            decoder_context,
            frame,
            options->target_timestamp,
            *target_packet_found,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            &output_written
        );
        if (ret < 0) {
            return ret;
        }
        if (output_written) {
            *target_frame_seen = 1;
        }
    }

    av_packet_unref(packet);
    av_frame_unref(frame);
    avcodec_flush_buffers(decoder_context);
    return 0;
}

static int build_packet_replay_cache(
    AVFormatContext *format_context,
    AVCodecContext *decoder_context,
    AVPacket *packet,
    AVFrame *frame,
    int video_stream_index,
    const Options *options,
    PacketReplayCache *cache
)
{
    int target_packet_found = 0;
    int target_frame_seen = 0;
    int final_target_packet_found = 0;
    int final_target_frame_seen = 0;
    int attempt = 0;
    int attempt_count = options->target_timestamp == 0 ? 1 : 2;
    int ret = 0;

    for (attempt = 0; attempt < attempt_count; attempt++) {
        int64_t seek_timestamp = attempt == 0 ? options->target_timestamp : 0;

        if (attempt > 0) {
            fprintf(
                stderr,
                "Retrying replay cache build from stream start for timestamp %" PRId64 "\n",
                options->target_timestamp
            );
        }

        ret = build_packet_replay_cache_attempt(
            format_context,
            decoder_context,
            packet,
            frame,
            video_stream_index,
            options,
            cache,
            seek_timestamp,
            &target_packet_found,
            &target_frame_seen
        );

        final_target_packet_found = target_packet_found;
        final_target_frame_seen = target_frame_seen;
        if (ret >= 0 && target_packet_found && target_frame_seen) {
            return 0;
        }
    }

    if (ret < 0) {
        return ret;
    }

    if (!final_target_packet_found) {
        fprintf(
            stderr,
            "Could not find a packet at byte offset %" PRId64 " while building replay cache for %s\n",
            options->target_packet_offset,
            options->input_path
        );
        return AVERROR(EINVAL);
    }
    if (!final_target_frame_seen) {
        fprintf(
            stderr,
            "Could not decode the target frame at timestamp %" PRId64 " while building replay cache\n",
            options->target_timestamp
        );
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int reset_video_decoder_for_trial(
    AVFormatContext *format_context,
    int *video_stream_index,
    AVCodecContext **decoder_context,
    AVFrame *frame,
    int export_motion_vectors
)
{
    /* Flush alone does not reset all custom decoder/concealment state. Start
     * every trial, including the first after cache construction, from a new
     * decoder so that packet batch boundaries cannot affect the result. */
    av_frame_unref(frame);
    avcodec_free_context(decoder_context);
    clear_packet_loss_corruption_env();
    return open_video_decoder(format_context, video_stream_index, decoder_context, export_motion_vectors);
}

/* The caller must reset_video_decoder_for_trial() before each replay. */
static int decode_target_frame_from_replay_cache(
    AVCodecContext *decoder_context,
    AVFrame *frame,
    const PacketReplayCache *cache,
    const Options *options,
    const SsimReferenceWindows *reference_windows,
    SsimWorkspace *ssim_workspace,
    double *ssim,
    BlockDamageMetrics *block_metrics,
    MotionVectorDumpContext *motion_vector_dump,
    GuessDcDumpContext *guess_dc_dump,
    GuessMvDumpContext *guess_mv_dump
)
{
    AVPacket *packet = NULL;
    int target_packet_found = 0;
    int output_written = 0;
    int packet_index = 0;
    int ret = 0;

    if (options->output_path != NULL) {
        remove(options->output_path);
    }
    av_frame_unref(frame);

    for (packet_index = 0; packet_index < cache->count && !output_written; packet_index++) {
        packet = av_packet_clone(cache->packets[packet_index]);
        if (packet == NULL) {
            ret = AVERROR(ENOMEM);
            goto done;
        }

        if (packet_index == cache->target_packet_index) {
            target_packet_found = 1;
            ret = corrupt_packet_payload(packet, options->transport_packet_size, options->corrupt_packet_index);
            if (ret < 0) {
                av_packet_free(&packet);
                goto done;
            }
        }

        ret = avcodec_send_packet(decoder_context, packet);
        av_packet_free(&packet);
        if (ret < 0) {
            fprintf(stderr, "Error while submitting a replay packet for decoding (%s)\n", av_err2str(ret));
            goto done;
        }

        ret = drain_decoder_until_target(
            decoder_context,
            frame,
            options->target_timestamp,
            target_packet_found,
            options->output_path,
            reference_windows,
            ssim_workspace,
            ssim,
            block_metrics,
            motion_vector_dump,
            guess_dc_dump,
            guess_mv_dump,
            &output_written
        );
        if (ret < 0) {
            goto done;
        }
    }

    if (!output_written) {
        ret = avcodec_send_packet(decoder_context, NULL);
        if (ret < 0) {
            fprintf(stderr, "Error while flushing the replay decoder (%s)\n", av_err2str(ret));
            goto done;
        }

        ret = drain_decoder_until_target(
            decoder_context,
            frame,
            options->target_timestamp,
            target_packet_found,
            options->output_path,
            reference_windows,
            ssim_workspace,
            ssim,
            block_metrics,
            motion_vector_dump,
            guess_dc_dump,
            guess_mv_dump,
            &output_written
        );
        if (ret < 0) {
            goto done;
        }
    }

    if (!target_packet_found) {
        fprintf(
            stderr,
            "Replay cache does not contain target packet at byte offset %" PRId64 "\n",
            options->target_packet_offset
        );
        ret = AVERROR(EINVAL);
        goto done;
    }

    if (!output_written) {
        fprintf(
            stderr,
            "Could not decode the target frame at timestamp %" PRId64 " from replay cache\n",
            options->target_timestamp
        );
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    ret = 0;

done:
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    av_frame_unref(frame);
    return ret < 0 ? ret : 0;
}

static int extract_target_frame(const Options *options)
{
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int video_stream_index = -1;
    int ret = 0;

    ret = avformat_open_input(&format_context, options->input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options->input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 0);
    if (ret < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = decode_target_frame(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        options,
        NULL,
        NULL,
        NULL,
        NULL
    );

cleanup:
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? ret : 0;
}

static char *build_batch_output_path(const char *output_dir, int packet_index, const char *output_extension)
{
    char *output_path = NULL;
    int required = snprintf(NULL, 0, "%s/packet_%04d.%s", output_dir, packet_index, output_extension);
    if (required < 0) {
        return NULL;
    }

    output_path = malloc((size_t)required + 1);
    if (output_path == NULL) {
        return NULL;
    }

    snprintf(output_path, (size_t)required + 1, "%s/packet_%04d.%s", output_dir, packet_index, output_extension);
    return output_path;
}

static int run_batch_mode(int argc, char **argv)
{
    Options options = {0};
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    const char *output_dir = NULL;
    const char *output_extension = "png";
    const char *ssim_reference_path = NULL;
    RgbImage reference_image = {0};
    SsimReferenceWindows ssim_reference = {0};
    SsimWorkspace ssim_workspace = {0};
    PacketReplayCache replay_cache = {0};
    int video_stream_index = -1;
    int start_corrupt_packet_index = 0;
    int stop_corrupt_packet_index = 0;
    int packet_index = 0;
    int ret = 0;

    if (argc != 9 && argc != 10 && argc != 11) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[2];
    output_dir = argv[3];
    if (parse_int64_arg(argv[4], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[5], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[6], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[7], "start_corrupt_packet_index", &start_corrupt_packet_index) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[8], "stop_corrupt_packet_index", &stop_corrupt_packet_index) < 0) {
        return 1;
    }
    if (start_corrupt_packet_index < 2 || stop_corrupt_packet_index < start_corrupt_packet_index) {
        fprintf(stderr, "Batch packet range must start at 2 or later and stop at or after start\n");
        return 1;
    }
    if (argc == 10) {
        output_extension = argv[9];
    }
    if (argc == 11) {
        output_extension = argv[9];
        ssim_reference_path = argv[10];
    }
    if (strcmp(output_extension, "png") != 0 && strcmp(output_extension, "ppm") != 0) {
        fprintf(stderr, "output_extension must be png or ppm\n");
        return 1;
    }
    if (ssim_reference_path != NULL && strcmp(output_extension, "ppm") != 0) {
        fprintf(stderr, "Native SSIM batch mode requires ppm output\n");
        return 1;
    }

    if (ssim_reference_path != NULL) {
        ret = load_ppm_image(ssim_reference_path, &reference_image);
        if (ret < 0) {
            goto cleanup;
        }
        ret = init_ssim_reference_windows(&reference_image, &ssim_reference);
        if (ret < 0) {
            goto cleanup;
        }
        ret = init_ssim_workspace(reference_image.width, reference_image.height, &ssim_workspace);
        if (ret < 0) {
            goto cleanup;
        }
    }

    ret = avformat_open_input(&format_context, options.input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options.input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 0);
    if (ret < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = build_packet_replay_cache(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        &options,
        &replay_cache
    );
    if (ret < 0) {
        goto cleanup;
    }

    printf("packet_index\tsuccess\toutput_path\tssim\n");
    for (packet_index = start_corrupt_packet_index; packet_index <= stop_corrupt_packet_index; packet_index++) {
        char *output_path = NULL;
        int decode_ret = 0;
        double ssim = NAN;

        ret = reset_video_decoder_for_trial(format_context, &video_stream_index, &decoder_context, frame, 0);
        if (ret < 0) {
            goto cleanup;
        }
        output_path = build_batch_output_path(output_dir, packet_index, output_extension);
        if (output_path == NULL) {
            fprintf(stderr, "Could not allocate output path for packet %d\n", packet_index);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        options.output_path = output_path;
        options.corrupt_packet_index = packet_index;
        fprintf(stderr, "[packet %d]\n", packet_index);
        decode_ret = decode_target_frame_from_replay_cache(
            decoder_context,
            frame,
            &replay_cache,
            &options,
            ssim_reference_path != NULL ? &ssim_reference : NULL,
            ssim_reference_path != NULL ? &ssim_workspace : NULL,
            &ssim,
            NULL,
            NULL,
            NULL,
            NULL
        );
        printf("%d\t%d\t%s\t%.17g\n", packet_index, decode_ret < 0 ? 0 : 1, output_path, ssim);
        fflush(stdout);
        free(output_path);
    }

cleanup:
    free_packet_replay_cache(&replay_cache);
    free_ssim_workspace(&ssim_workspace);
    free_ssim_reference_windows(&ssim_reference);
    free_rgb_image(&reference_image);
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? 1 : 0;
}

static int run_error_blocks_mode(int argc, char **argv)
{
    Options options = {0};
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    PacketReplayCache replay_cache = {0};
    int video_stream_index = -1;
    int start_corrupt_packet_index = 0;
    int stop_corrupt_packet_index = 0;
    int packet_index = 0;
    int ret = 0;

    if (argc != 8) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[2];
    options.output_path = NULL;
    if (parse_int64_arg(argv[3], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[4], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[5], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[6], "start_corrupt_packet_index", &start_corrupt_packet_index) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[7], "stop_corrupt_packet_index", &stop_corrupt_packet_index) < 0) {
        return 1;
    }
    if (start_corrupt_packet_index < 2 || stop_corrupt_packet_index < start_corrupt_packet_index) {
        fprintf(stderr, "Error-block packet range must start at 2 or later and stop at or after start\n");
        return 1;
    }

    ret = avformat_open_input(&format_context, options.input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options.input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 0);
    if (ret < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = build_packet_replay_cache(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        &options,
        &replay_cache
    );
    if (ret < 0) {
        goto cleanup;
    }

    printf(
        "packet_index\tsuccess\tblock_unit\tblock_width\tblock_height\ttotal_blocks\t"
        "damaged_blocks\tconcealed_blocks\tinter_concealed_blocks\tdamaged_block_ratio\t"
        "decode_error_flags\tframe_corrupt\trecovery_total_mbs\trecovery_decoded_ok_mbs\t"
        "recovery_mv_guessed_mbs\trecovery_spatial_concealed_mbs\t"
        "recovery_reference_copied_mbs\trecovery_failed_or_unclassified_mbs\n"
    );
    for (packet_index = start_corrupt_packet_index; packet_index <= stop_corrupt_packet_index; packet_index++) {
        BlockDamageMetrics metrics;
        int decode_ret = 0;

        ret = reset_video_decoder_for_trial(format_context, &video_stream_index, &decoder_context, frame, 0);
        if (ret < 0) {
            goto cleanup;
        }
        reset_block_damage_metrics(&metrics);
        options.corrupt_packet_index = packet_index;
        fprintf(stderr, "[packet %d]\n", packet_index);
        decode_ret = decode_target_frame_from_replay_cache(
            decoder_context,
            frame,
            &replay_cache,
            &options,
            NULL,
            NULL,
            NULL,
            &metrics,
            NULL,
            NULL,
            NULL
        );
        print_block_damage_result(packet_index, decode_ret < 0 ? 0 : 1, &metrics);
        fflush(stdout);
    }

cleanup:
    free_packet_replay_cache(&replay_cache);
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? 1 : 0;
}

static int run_mv_dump_mode(int argc, char **argv)
{
    Options options = {0};
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    PacketReplayCache replay_cache = {0};
    int video_stream_index = -1;
    int start_corrupt_packet_index = 0;
    int stop_corrupt_packet_index = 0;
    int packet_index = 0;
    int ret = 0;

    if (argc != 8) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[2];
    options.output_path = NULL;
    if (parse_int64_arg(argv[3], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[4], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[5], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[6], "start_corrupt_packet_index", &start_corrupt_packet_index) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[7], "stop_corrupt_packet_index", &stop_corrupt_packet_index) < 0) {
        return 1;
    }
    if (!((start_corrupt_packet_index == 0 && stop_corrupt_packet_index == 0) ||
          (start_corrupt_packet_index >= 2 && stop_corrupt_packet_index >= start_corrupt_packet_index))) {
        fprintf(stderr, "MV dump packet range must be 0..0 for clean decode or start at 2 or later\n");
        return 1;
    }

    ret = avformat_open_input(&format_context, options.input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options.input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 1);
    if (ret < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = build_packet_replay_cache(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        &options,
        &replay_cache
    );
    if (ret < 0) {
        goto cleanup;
    }

    printf(
        "packet_index\tsuccess\tframe_timestamp\tsource\tdst_x\tdst_y\tmb_x\tmb_y\t"
        "motion_x\tmotion_y\tmotion_scale\tsrc_x\tsrc_y\tsource_ref\tw\th\tflags\t"
        "er_origin_mb_x\ter_origin_mb_y\ter_total_blocks\ter_damaged_mb_count\tmanhattan_to_origin\t"
        "decode_error_flags\tframe_corrupt\n"
    );
    for (packet_index = start_corrupt_packet_index; packet_index <= stop_corrupt_packet_index; packet_index++) {
        MotionVectorDumpContext motion_vector_dump = {0};
        int decode_ret = 0;

        ret = reset_video_decoder_for_trial(format_context, &video_stream_index, &decoder_context, frame, 1);
        if (ret < 0) {
            goto cleanup;
        }
        options.corrupt_packet_index = packet_index;
        motion_vector_dump.packet_index = packet_index;
        motion_vector_dump.output = stdout;
        fprintf(stderr, "[packet %d]\n", packet_index);
        decode_ret = decode_target_frame_from_replay_cache(
            decoder_context,
            frame,
            &replay_cache,
            &options,
            NULL,
            NULL,
            NULL,
            NULL,
            &motion_vector_dump,
            NULL,
            NULL
        );
        if (decode_ret < 0) {
            fprintf(stderr, "MV dump decode failed for packet %d\n", packet_index);
        }
        fflush(stdout);
    }

cleanup:
    free_packet_replay_cache(&replay_cache);
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? 1 : 0;
}

static void clear_guess_dc_env(void)
{
    unsetenv("PACKET_LOSS_GUESS_DC_DUMP");
}

static void clear_guess_mv_env(void)
{
    unsetenv("PACKET_LOSS_GUESS_MV_DUMP");
    unsetenv("PACKET_LOSS_GUESS_MV_TRUTH_DUMP");
}

static int run_guess_dc_dump_mode(int argc, char **argv)
{
    Options options = {0};
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    PacketReplayCache replay_cache = {0};
    int video_stream_index = -1;
    int start_corrupt_packet_index = 0;
    int stop_corrupt_packet_index = 0;
    int packet_index = 0;
    int clean_mode = 0;
    int ret = 0;

    if (argc != 8) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[2];
    options.output_path = NULL;
    if (parse_int64_arg(argv[3], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[4], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[5], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[6], "start_corrupt_packet_index", &start_corrupt_packet_index) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[7], "stop_corrupt_packet_index", &stop_corrupt_packet_index) < 0) {
        return 1;
    }
    clean_mode = start_corrupt_packet_index == 0 && stop_corrupt_packet_index == 0;
    if (!(clean_mode || (start_corrupt_packet_index >= 2 && stop_corrupt_packet_index >= start_corrupt_packet_index))) {
        fprintf(stderr, "guess_dc dump packet range must be 0..0 for clean decode or start at 2 or later\n");
        return 1;
    }

    ret = avformat_open_input(&format_context, options.input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options.input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 0);
    if (ret < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = build_packet_replay_cache(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        &options,
        &replay_cache
    );
    if (ret < 0) {
        goto cleanup;
    }

    if (clean_mode) {
        printf("packet_index\tsuccess\tframe_timestamp\tcomponent\tblock_x\tblock_y\tmb_x\tmb_y\tclean_dc\n");
    } else {
        printf(
            "packet_index\tsuccess\tframe_timestamp\tcomponent\tblock_x\tblock_y\tmb_x\tmb_y\t"
            "dist0\tdist1\tdist2\tdist3\tweight0\tweight1\tweight2\tweight3\t"
            "boundary_dc0\tboundary_dc1\tboundary_dc2\tboundary_dc3\tguess_dc\tpre_guess_dc\n"
        );
    }

    for (packet_index = start_corrupt_packet_index; packet_index <= stop_corrupt_packet_index; packet_index++) {
        GuessDcDumpContext guess_dc_dump = {0};
        int decode_ret = 0;

        clear_guess_dc_env();
        if (!clean_mode) {
            setenv("PACKET_LOSS_GUESS_DC_DUMP", "1", 1);
        }
        ret = reset_video_decoder_for_trial(format_context, &video_stream_index, &decoder_context, frame, 0);
        if (ret < 0) {
            goto cleanup;
        }
        options.corrupt_packet_index = packet_index;
        guess_dc_dump.packet_index = packet_index;
        guess_dc_dump.output = stdout;
        guess_dc_dump.clean_mode = clean_mode;
        fprintf(stderr, "[packet %d]\n", packet_index);
        decode_ret = decode_target_frame_from_replay_cache(
            decoder_context,
            frame,
            &replay_cache,
            &options,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            &guess_dc_dump,
            NULL
        );
        if (decode_ret < 0) {
            fprintf(stderr, "guess_dc dump decode failed for packet %d\n", packet_index);
        }
        fflush(stdout);
    }

cleanup:
    clear_guess_dc_env();
    free_packet_replay_cache(&replay_cache);
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? 1 : 0;
}

static int run_guess_mv_dump_mode(int argc, char **argv)
{
    Options options = {0};
    AVFormatContext *format_context = NULL;
    AVCodecContext *decoder_context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    PacketReplayCache replay_cache = {0};
    int video_stream_index = -1;
    int start_corrupt_packet_index = 0;
    int stop_corrupt_packet_index = 0;
    int packet_index = 0;
    int clean_mode = 0;
    int ret = 0;

    if (argc != 8) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[2];
    options.output_path = NULL;
    if (parse_int64_arg(argv[3], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[4], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[5], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[6], "start_corrupt_packet_index", &start_corrupt_packet_index) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[7], "stop_corrupt_packet_index", &stop_corrupt_packet_index) < 0) {
        return 1;
    }
    clean_mode = start_corrupt_packet_index == 0 && stop_corrupt_packet_index == 0;
    if (!(clean_mode || (start_corrupt_packet_index >= 2 && stop_corrupt_packet_index >= start_corrupt_packet_index))) {
        fprintf(stderr, "guess_mv dump packet range must be 0..0 for clean decode or start at 2 or later\n");
        return 1;
    }

    ret = avformat_open_input(&format_context, options.input_path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file %s (%s)\n", options.input_path, av_err2str(ret));
        goto cleanup;
    }

    ret = avformat_find_stream_info(format_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information (%s)\n", av_err2str(ret));
        goto cleanup;
    }

    ret = open_video_decoder(format_context, &video_stream_index, &decoder_context, 1);
    if (ret < 0) {
        goto cleanup;
    }
    if (decoder_context->codec_id != AV_CODEC_ID_H264 &&
        decoder_context->codec_id != AV_CODEC_ID_HEVC &&
        decoder_context->codec_id != AV_CODEC_ID_VP8 &&
        decoder_context->codec_id != AV_CODEC_ID_VP9 &&
        decoder_context->codec_id != AV_CODEC_ID_VVC &&
        decoder_context->codec_id != AV_CODEC_ID_AV1) {
        fprintf(stderr, "guess_mv dump currently supports only H.264/VP8/VP9/AV1/H.265/H.266 input\n");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }

    clear_guess_mv_env();
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = build_packet_replay_cache(
        format_context,
        decoder_context,
        packet,
        frame,
        video_stream_index,
        &options,
        &replay_cache
    );
    if (ret < 0) {
        goto cleanup;
    }

    if (clean_mode) {
        printf("packet_index\tsuccess\tframe_timestamp\tmb_x\tmb_y\ttruth_mv_x\ttruth_mv_y\ttruth_ref\tmotion_scale\n");
    } else {
        printf(
            "packet_index\tsuccess\tframe_timestamp\tmb_x\tmb_y\thop_count\tpass_index\t"
            "pred_count\testimated_mv_x\testimated_mv_y\testimated_ref\tbest_score\n"
        );
    }

    for (packet_index = start_corrupt_packet_index; packet_index <= stop_corrupt_packet_index; packet_index++) {
        GuessMvDumpContext guess_mv_dump = {0};
        int decode_ret = 0;

        clear_guess_mv_env();
        if (clean_mode) {
            setenv("PACKET_LOSS_GUESS_MV_TRUTH_DUMP", "1", 1);
        } else {
            setenv("PACKET_LOSS_GUESS_MV_DUMP", "1", 1);
        }
        ret = reset_video_decoder_for_trial(format_context, &video_stream_index, &decoder_context, frame, 1);
        if (ret < 0) {
            goto cleanup;
        }
        options.corrupt_packet_index = packet_index;
        guess_mv_dump.packet_index = packet_index;
        guess_mv_dump.output = stdout;
        guess_mv_dump.clean_mode = clean_mode;
        fprintf(stderr, "[packet %d]\n", packet_index);
        decode_ret = decode_target_frame_from_replay_cache(
            decoder_context,
            frame,
            &replay_cache,
            &options,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            &guess_mv_dump
        );
        if (decode_ret < 0) {
            fprintf(stderr, "guess_mv dump decode failed for packet %d\n", packet_index);
        }
        fflush(stdout);
    }

cleanup:
    clear_guess_mv_env();
    free_packet_replay_cache(&replay_cache);
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (decoder_context != NULL) {
        avcodec_free_context(&decoder_context);
    }
    if (format_context != NULL) {
        avformat_close_input(&format_context);
    }
    return ret < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    Options options = {0};
    int ret = 0;

    if (argc > 1 && strcmp(argv[1], "--batch") == 0) {
        return run_batch_mode(argc, argv);
    }
    if (argc > 1 && strcmp(argv[1], "--error-blocks") == 0) {
        return run_error_blocks_mode(argc, argv);
    }
    if (argc > 1 && strcmp(argv[1], "--mv-dump") == 0) {
        return run_mv_dump_mode(argc, argv);
    }
    if (argc > 1 && strcmp(argv[1], "--guess-dc-dump") == 0) {
        return run_guess_dc_dump_mode(argc, argv);
    }
    if (argc > 1 && strcmp(argv[1], "--guess-mv-dump") == 0) {
        return run_guess_mv_dump_mode(argc, argv);
    }

    if (argc != 6 && argc != 7) {
        usage(argv[0]);
        return 1;
    }

    options.input_path = argv[1];
    options.output_path = argv[2];
    if (parse_int64_arg(argv[3], "target_timestamp", &options.target_timestamp) < 0) {
        return 1;
    }
    if (parse_int64_arg(argv[4], "target_packet_offset", &options.target_packet_offset) < 0) {
        return 1;
    }
    if (parse_int_arg(argv[5], "transport_packet_size", &options.transport_packet_size) < 0) {
        return 1;
    }
    options.corrupt_packet_index = 0;
    if (argc == 7 && parse_int_arg(argv[6], "corrupt_packet_index", &options.corrupt_packet_index) < 0) {
        return 1;
    }

    ret = extract_target_frame(&options);
    return ret < 0 ? 1 : 0;
}
