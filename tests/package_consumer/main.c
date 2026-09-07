#include <turbo_pipeline.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    static const char yaml[] =
        "api_version: turbo.media.pipeline/v1\n"
        "id: package-consumer\n"
        "nodes:\n"
        "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: input.ts } }\n"
        "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n"
        "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: mpegts } }\n"
        "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: output.ts } }\n"
        "edges:\n"
        "  - { from: source.out, to: demux.in }\n"
        "  - { from: demux.video, to: mux.video }\n"
        "  - { from: mux.out, to: sink.in }\n";
    turbo_pipeline_error_t error;
    turbo_pipeline_t *pipeline =
        turbo_pipeline_create_from_yaml(yaml, strlen(yaml), &error);

    if (!pipeline) {
        fprintf(stderr, "pipeline create failed: code=%d node=%s message=%s\n",
                error.code, error.node_id, error.message);
        return 1;
    }
    if (pipeline &&
        turbo_pipeline_bind_server_runtime(pipeline, NULL, &error) !=
            TURBO_PIPELINE_EINVAL) {
        turbo_pipeline_destroy(pipeline);
        return 2;
    }
    turbo_pipeline_destroy(pipeline);
    return 0;
}
