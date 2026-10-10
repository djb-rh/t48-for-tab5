// One ROM's image builder and firmware parser (onerom-ffi, Rust; see
// onerom-ffi/src/lib.rs for what each call does). Text results are
// "key=value" or tab-separated lines; buffers come back through ort_free.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OrtBuilder OrtBuilder;
typedef int (*OrtReadFn)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);

size_t ort_versions(char *out, size_t cap);
size_t ort_chip_types(const char *board, char *out, size_t cap);
size_t ort_chip_info(const char *name, char *out, size_t cap);
size_t ort_pick_firmware(const char *manifest, size_t len, const char *board, char *out, size_t cap);
size_t ort_pick_plugin(const char *manifest, size_t len, const char *base_url, uint16_t maj, uint16_t min,
                       uint16_t patch, char *out, size_t cap);

OrtBuilder *ort_builder_new(const char *json, size_t json_len, uint16_t maj, uint16_t min, uint16_t patch,
                            char *err, size_t err_cap);
void ort_builder_free(OrtBuilder *b);
size_t ort_builder_files(OrtBuilder *b, char *out, size_t cap);
int ort_builder_add_file(OrtBuilder *b, size_t id, const uint8_t *data, size_t len, char *err, size_t err_cap);
int ort_builder_build(OrtBuilder *b, const char *board, const char *size, uint8_t **meta, size_t *meta_len,
                      uint8_t **image, size_t *image_len, char *err, size_t err_cap);
size_t ort_builder_description(OrtBuilder *b, const char *board, char *out, size_t cap);
int ort_assemble(const uint8_t *fw, size_t fw_len, const uint8_t *meta, size_t meta_len, const uint8_t *image,
                 size_t image_len, uint8_t **out, size_t *out_len);
void ort_free(uint8_t *p, size_t len);

size_t ort_parse_device(OrtReadFn read, void *ctx, char *out, size_t cap);

// Supplied by the firmware: a Rust panic, reported and never returned from.
void ort_panic(const uint8_t *msg, size_t len);

#ifdef __cplusplus
}
#endif
