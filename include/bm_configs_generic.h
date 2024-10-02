#include "util.h"
#include <stdint.h>

#define BM_MAX_KEY_LEN_BYTES 32

typedef enum ConfigDataTypes {
  UINT32,
  INT32,
  FLOAT,
  STR,
  BYTES,
  ARRAY,
} ConfigDataTypes;

typedef struct ConfigKey {
  char keyBuffer[BM_MAX_KEY_LEN_BYTES];
  size_t keyLen;
  ConfigDataTypes valueType;
} __attribute__((packed, aligned(1))) ConfigKey;

const ConfigKey *bcmp_config_get_stored_keys(uint8_t &num_stored_keys);
bool bcmp_remove_key(const char *key, size_t key_len);
// TODO - converge on commit vs save as the naming convention!
bool bcmp_config_needs_commit(void);
bool bcmp_save_config(bool restart = true);
bool bcmp_set_config(const char *key, size_t key_len, uint8_t *value,
                     size_t value_len);
bool bcmp_get_config(const char *key, size_t key_len, uint8_t *value,
                     size_t &value_len);
