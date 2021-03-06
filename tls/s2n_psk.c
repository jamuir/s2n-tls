/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "crypto/s2n_tls13_keys.h"

#include "tls/s2n_handshake.h"
#include "tls/s2n_tls13_handshake.h"
#include "tls/s2n_tls.h"
#include "tls/extensions/s2n_extension_type.h"

#include "utils/s2n_array.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

#define S2N_HASH_ALG_COUNT S2N_HASH_SENTINEL

S2N_RESULT s2n_psk_init(struct s2n_psk *psk, s2n_psk_type type)
{
    ENSURE_MUT(psk);

    CHECKED_MEMSET(psk, 0, sizeof(struct s2n_psk));
    psk->hmac_alg = S2N_HMAC_SHA256;
    psk->type = type;

    return S2N_RESULT_OK;
}

struct s2n_psk* s2n_external_psk_new()
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    GUARD_PTR(s2n_alloc(&mem, sizeof(struct s2n_psk)));

    struct s2n_psk *psk = (struct s2n_psk*)(void*) mem.data;
    GUARD_RESULT_PTR(s2n_psk_init(psk, S2N_PSK_TYPE_EXTERNAL));

    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return psk;
}

int s2n_psk_set_identity(struct s2n_psk *psk, const uint8_t *identity, uint16_t identity_size)
{
    notnull_check(psk);
    notnull_check(identity);
    ENSURE_POSIX(identity_size != 0, S2N_ERR_INVALID_ARGUMENT);

    GUARD(s2n_realloc(&psk->identity, identity_size));
    memcpy_check(psk->identity.data, identity, identity_size);

    return S2N_SUCCESS;
}

int s2n_psk_set_secret(struct s2n_psk *psk, const uint8_t *secret, uint16_t secret_size)
{
    notnull_check(psk);
    notnull_check(secret);
    ENSURE_POSIX(secret_size != 0, S2N_ERR_INVALID_ARGUMENT);

    GUARD(s2n_realloc(&psk->secret, secret_size));
    memcpy_check(psk->secret.data, secret, secret_size);

    return S2N_SUCCESS;
}

S2N_RESULT s2n_psk_clone(struct s2n_psk *new_psk, struct s2n_psk *original_psk)
{
    if (original_psk == NULL) {
        return S2N_RESULT_OK;
    }
    ENSURE_REF(new_psk);

    struct s2n_psk psk_copy = *new_psk;

    /* Copy all fields from the old_config EXCEPT the blobs, which we need to reallocate. */
    *new_psk = *original_psk;
    new_psk->identity = psk_copy.identity;
    new_psk->secret = psk_copy.secret;
    new_psk->early_secret = psk_copy.early_secret;
    new_psk->early_data_config = psk_copy.early_data_config;

    /* Clone / realloc blobs */
    GUARD_AS_RESULT(s2n_psk_set_identity(new_psk, original_psk->identity.data, original_psk->identity.size));
    GUARD_AS_RESULT(s2n_psk_set_secret(new_psk, original_psk->secret.data, original_psk->secret.size));
    GUARD_AS_RESULT(s2n_realloc(&new_psk->early_secret, original_psk->early_secret.size));
    CHECKED_MEMCPY(new_psk->early_secret.data, original_psk->early_secret.data, original_psk->early_secret.size);
    GUARD_RESULT(s2n_early_data_config_clone(new_psk, &original_psk->early_data_config));

    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_psk_wipe(struct s2n_psk *psk)
{
    if (psk == NULL) {
        return S2N_RESULT_OK;
    }

    GUARD_AS_RESULT(s2n_free(&psk->early_secret));
    GUARD_AS_RESULT(s2n_free(&psk->identity));
    GUARD_AS_RESULT(s2n_free(&psk->secret));
    GUARD_RESULT(s2n_early_data_config_free(&psk->early_data_config));

    return S2N_RESULT_OK;
}

int s2n_psk_free(struct s2n_psk **psk)
{
    if (psk == NULL) {
        return S2N_SUCCESS;
    }
    GUARD_AS_POSIX(s2n_psk_wipe(*psk));
    return s2n_free_object((uint8_t **) psk, sizeof(struct s2n_psk));
}

S2N_RESULT s2n_psk_parameters_init(struct s2n_psk_parameters *params)
{
    ENSURE_REF(params);
    CHECKED_MEMSET(params, 0, sizeof(struct s2n_psk_parameters));
    GUARD_RESULT(s2n_array_init(&params->psk_list, sizeof(struct s2n_psk)));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_psk_offered_psk_size(struct s2n_psk *psk, uint32_t *size)
{
    *size = sizeof(uint16_t)    /* identity size */
          + sizeof(uint32_t)    /* obfuscated ticket age */
          + sizeof(uint8_t)     /* binder size */;

    GUARD_AS_RESULT(s2n_add_overflow(*size, psk->identity.size, size));

    uint8_t binder_size = 0;
    GUARD_AS_RESULT(s2n_hmac_digest_size(psk->hmac_alg, &binder_size));
    GUARD_AS_RESULT(s2n_add_overflow(*size, binder_size, size));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_psk_parameters_offered_psks_size(struct s2n_psk_parameters *params, uint32_t *size)
{
    ENSURE_REF(params);
    ENSURE_REF(size);

    *size = sizeof(uint16_t)    /* identity list size */
          + sizeof(uint16_t)    /* binder list size */;

    for (uint32_t i = 0; i < params->psk_list.len; i++) {
        struct s2n_psk *psk = NULL;
        GUARD_RESULT(s2n_array_get(&params->psk_list, i, (void**)&psk));
        ENSURE_REF(psk);

        uint32_t psk_size = 0;
        GUARD_RESULT(s2n_psk_offered_psk_size(psk, &psk_size));
        GUARD_AS_RESULT(s2n_add_overflow(*size, psk_size, size));
    }
    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_psk_parameters_wipe(struct s2n_psk_parameters *params)
{
    ENSURE_REF(params);

    for (size_t i = 0; i < params->psk_list.len; i++) {
        struct s2n_psk *psk;
        GUARD_RESULT(s2n_array_get(&params->psk_list, i, (void**)&psk));
        GUARD_RESULT(s2n_psk_wipe(psk));
    }
    GUARD_AS_RESULT(s2n_free(&params->psk_list.mem));
    GUARD_RESULT(s2n_psk_parameters_init(params));

    return S2N_RESULT_OK;
}

bool s2n_offered_psk_list_has_next(struct s2n_offered_psk_list *psk_list)
{
    return psk_list != NULL && s2n_stuffer_data_available(&psk_list->wire_data) > 0;
}

S2N_RESULT s2n_offered_psk_list_read_next(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk)
{
    ENSURE_REF(psk_list);
    ENSURE_MUT(psk);

    uint16_t identity_size = 0;
    GUARD_AS_RESULT(s2n_stuffer_read_uint16(&psk_list->wire_data, &identity_size));
    ENSURE_GT(identity_size, 0);

    uint8_t *identity_data = NULL;
    identity_data = s2n_stuffer_raw_read(&psk_list->wire_data, identity_size);
    ENSURE_REF(identity_data);

    /**
     *= https://tools.ietf.org/rfc/rfc8446#section-4.2.11
     *# For identities established externally, an obfuscated_ticket_age of 0 SHOULD be
     *# used, and servers MUST ignore the value.
     */
    GUARD_AS_RESULT(s2n_stuffer_skip_read(&psk_list->wire_data, sizeof(uint32_t)));

    GUARD_AS_RESULT(s2n_blob_init(&psk->identity, identity_data, identity_size));
    return S2N_RESULT_OK;
}

int s2n_offered_psk_list_next(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk)
{
    notnull_check(psk_list);
    notnull_check(psk);
    *psk = (struct s2n_offered_psk){ 0 };
    ENSURE_POSIX(s2n_offered_psk_list_has_next(psk_list), S2N_ERR_STUFFER_OUT_OF_DATA);
    ENSURE_POSIX(s2n_result_is_ok(s2n_offered_psk_list_read_next(psk_list, psk)), S2N_ERR_BAD_MESSAGE);
    return S2N_SUCCESS;
}

int s2n_offered_psk_list_reset(struct s2n_offered_psk_list *psk_list)
{
    notnull_check(psk_list);
    return s2n_stuffer_reread(&psk_list->wire_data);
}

S2N_RESULT s2n_offered_psk_list_get_index(struct s2n_offered_psk_list *psk_list, uint16_t psk_index, struct s2n_offered_psk *psk)
{
    ENSURE_REF(psk_list);
    ENSURE_MUT(psk);

    /* We don't want to lose our original place in the list, so copy it */
    struct s2n_offered_psk_list psk_list_copy = { .wire_data = psk_list->wire_data };
    GUARD_AS_RESULT(s2n_offered_psk_list_reset(&psk_list_copy));

    uint16_t count = 0;
    while(count <= psk_index) {
        GUARD_AS_RESULT(s2n_offered_psk_list_next(&psk_list_copy, psk));
        count++;
    }
    return S2N_RESULT_OK;
}

struct s2n_offered_psk* s2n_offered_psk_new()
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    GUARD_PTR(s2n_alloc(&mem, sizeof(struct s2n_offered_psk)));
    GUARD_PTR(s2n_blob_zero(&mem));

    struct s2n_offered_psk *psk = (struct s2n_offered_psk*)(void*) mem.data;

    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return psk;
}

int s2n_offered_psk_free(struct s2n_offered_psk **psk)
{
    if (psk == NULL) {
        return S2N_SUCCESS;
    }
    return s2n_free_object((uint8_t **) psk, sizeof(struct s2n_offered_psk));
}

int s2n_offered_psk_get_identity(struct s2n_offered_psk *psk, uint8_t** identity, uint16_t *size)
{
    notnull_check(psk);
    notnull_check(identity);
    notnull_check(size);
    *identity = psk->identity.data;
    *size = psk->identity.size;
    return S2N_SUCCESS;
}

/* The binder hash is computed by hashing the concatenation of the current transcript
 * and a partial ClientHello that does not include the binders themselves.
 */
int s2n_psk_calculate_binder_hash(struct s2n_connection *conn, s2n_hmac_algorithm hmac_alg,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *output_binder_hash)
{
    notnull_check(partial_client_hello);
    notnull_check(output_binder_hash);

    /* Retrieve the current transcript.
     * The current transcript will be empty unless this handshake included a HelloRetryRequest. */
    struct s2n_hash_state current_hash_state = {0};

    s2n_hash_algorithm hash_alg;
    GUARD(s2n_hmac_hash_alg(hmac_alg, &hash_alg));
    GUARD(s2n_handshake_get_hash_state(conn, hash_alg, &current_hash_state));

    /* Copy the current transcript to avoid modifying the original. */
    DEFER_CLEANUP(struct s2n_hash_state hash_copy, s2n_hash_free);
    GUARD(s2n_hash_new(&hash_copy));
    GUARD(s2n_hash_copy(&hash_copy, &current_hash_state));

    /* Add the partial client hello to the transcript. */
    GUARD(s2n_hash_update(&hash_copy, partial_client_hello->data, partial_client_hello->size));

    /* Get the transcript digest */
    GUARD(s2n_hash_digest(&hash_copy, output_binder_hash->data, output_binder_hash->size));

    return S2N_SUCCESS;
}

/* The binder is computed in the same way as the Finished message
 * (https://tools.ietf.org/html/rfc8446#section-4.4.4) but with the BaseKey being the binder_key
 * derived via the key schedule from the corresponding PSK which is being offered
 * (https://tools.ietf.org/html/rfc8446#section-7.1)
 */
int s2n_psk_calculate_binder(struct s2n_psk *psk, const struct s2n_blob *binder_hash,
        struct s2n_blob *output_binder)
{
    notnull_check(psk);
    notnull_check(binder_hash);
    notnull_check(output_binder);

    DEFER_CLEANUP(struct s2n_tls13_keys psk_keys, s2n_tls13_keys_free);
    GUARD(s2n_tls13_keys_init(&psk_keys, psk->hmac_alg));
    eq_check(binder_hash->size, psk_keys.size);
    eq_check(output_binder->size, psk_keys.size);

    /* Make sure the early secret is saved on the psk structure for later use */
    GUARD(s2n_realloc(&psk->early_secret, psk_keys.size));
    GUARD(s2n_blob_init(&psk_keys.extract_secret, psk->early_secret.data, psk_keys.size));

    /* Derive the binder key */
    GUARD(s2n_tls13_derive_binder_key(&psk_keys, psk));
    struct s2n_blob *binder_key = &psk_keys.derive_secret;

    /* Expand the binder key into the finished key */
    s2n_tls13_key_blob(finished_key, psk_keys.size);
    GUARD(s2n_tls13_derive_finished_key(&psk_keys, binder_key, &finished_key));

    /* HMAC the binder hash with the binder finished key */
    GUARD(s2n_hkdf_extract(&psk_keys.hmac, psk_keys.hmac_algorithm, &finished_key, binder_hash, output_binder));

    return S2N_SUCCESS;
}

int s2n_psk_verify_binder(struct s2n_connection *conn, struct s2n_psk *psk,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *binder_to_verify)
{
    notnull_check(psk);
    notnull_check(binder_to_verify);

    DEFER_CLEANUP(struct s2n_tls13_keys psk_keys, s2n_tls13_keys_free);
    GUARD(s2n_tls13_keys_init(&psk_keys, psk->hmac_alg));
    eq_check(binder_to_verify->size, psk_keys.size);

    /* Calculate the binder hash from the transcript */
    s2n_tls13_key_blob(binder_hash, psk_keys.size);
    GUARD(s2n_psk_calculate_binder_hash(conn, psk->hmac_alg, partial_client_hello, &binder_hash));

    /* Calculate the expected binder from the binder hash */
    s2n_tls13_key_blob(expected_binder, psk_keys.size);
    GUARD(s2n_psk_calculate_binder(psk, &binder_hash, &expected_binder));

    /* Verify the expected binder matches the given binder.
     * This operation must be constant time. */
    GUARD(s2n_tls13_mac_verify(&psk_keys, &expected_binder, binder_to_verify));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_psk_write_binder(struct s2n_connection *conn, struct s2n_psk *psk,
        const struct s2n_blob *binder_hash, struct s2n_stuffer *out)
{
    ENSURE_REF(binder_hash);

    struct s2n_blob binder;
    uint8_t binder_data[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    GUARD_AS_RESULT(s2n_blob_init(&binder, binder_data, binder_hash->size));

    GUARD_AS_RESULT(s2n_psk_calculate_binder(psk, binder_hash, &binder));
    GUARD_AS_RESULT(s2n_stuffer_write_uint8(out, binder.size));
    GUARD_AS_RESULT(s2n_stuffer_write(out, &binder));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_psk_write_binder_list(struct s2n_connection *conn, const struct s2n_blob *partial_client_hello,
        struct s2n_stuffer *out)
{
    ENSURE_REF(conn);
    ENSURE_REF(partial_client_hello);

    struct s2n_psk_parameters *psk_params = &conn->psk_params;
    struct s2n_array *psk_list = &psk_params->psk_list;

    /* Setup memory to hold the binder hashes. We potentially need one for
     * every hash algorithm. */
    uint8_t binder_hashes_data[S2N_HASH_ALG_COUNT][S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    struct s2n_blob binder_hashes[S2N_HASH_ALG_COUNT] = { 0 };

    struct s2n_stuffer_reservation binder_list_size = { 0 };
    GUARD_AS_RESULT(s2n_stuffer_reserve_uint16(out, &binder_list_size));

    /* Write binder for every psk */
    for (size_t i = 0; i < psk_list->len; i++) {
        struct s2n_psk *psk = NULL;
        GUARD_RESULT(s2n_array_get(psk_list, i, (void**) &psk));
        ENSURE_REF(psk);

        /**
         *= https://tools.ietf.org/rfc/rfc8446#section-4.1.4
         *# In addition, in its updated ClientHello, the client SHOULD NOT offer
         *# any pre-shared keys associated with a hash other than that of the
         *# selected cipher suite.  This allows the client to avoid having to
         *# compute partial hash transcripts for multiple hashes in the second
         *# ClientHello.
         */
        if (s2n_is_hello_retry_handshake(conn) && conn->secure.cipher_suite->prf_alg != psk->hmac_alg) {
            continue;
        }

        /* Retrieve or calculate the binder hash. */
        struct s2n_blob *binder_hash = &binder_hashes[psk->hmac_alg];
        if (binder_hash->size == 0) {
            uint8_t hash_size = 0;
            GUARD_AS_RESULT(s2n_hmac_digest_size(psk->hmac_alg, &hash_size));
            GUARD_AS_RESULT(s2n_blob_init(binder_hash, binder_hashes_data[psk->hmac_alg], hash_size));
            GUARD_AS_RESULT(s2n_psk_calculate_binder_hash(conn, psk->hmac_alg, partial_client_hello, binder_hash));
        }

        GUARD_RESULT(s2n_psk_write_binder(conn, psk, binder_hash, out));
    }
    GUARD_AS_RESULT(s2n_stuffer_write_vector_size(&binder_list_size));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_finish_psk_extension(struct s2n_connection *conn)
{
    ENSURE_REF(conn);

    if (!conn->psk_params.binder_list_size) {
        return S2N_RESULT_OK;
    }

    struct s2n_stuffer *client_hello = &conn->handshake.io;
    struct s2n_psk_parameters *psk_params = &conn->psk_params;

    /* Fill in the correct message size. */
    GUARD_AS_RESULT(s2n_handshake_finish_header(client_hello));

    /* Remove the empty space allocated for the binder list.
     * It was originally added to ensure the extension / extension list / message sizes
     * were properly calculated. */
    GUARD_AS_RESULT(s2n_stuffer_wipe_n(client_hello, psk_params->binder_list_size));

    /* Store the partial client hello for use in calculating the binder hash. */
    struct s2n_blob partial_client_hello = { 0 };
    GUARD_AS_RESULT(s2n_blob_init(&partial_client_hello, client_hello->blob.data,
            s2n_stuffer_data_available(client_hello)));

    GUARD_RESULT(s2n_psk_write_binder_list(conn, &partial_client_hello, client_hello));
    return S2N_RESULT_OK;
}

int s2n_psk_set_hmac(struct s2n_psk *psk, s2n_psk_hmac hmac)
{
    notnull_check(psk);
    switch(hmac) {
        case S2N_PSK_HMAC_SHA224:     psk->hmac_alg = S2N_HMAC_SHA224; break;
        case S2N_PSK_HMAC_SHA256:     psk->hmac_alg = S2N_HMAC_SHA256; break;
        case S2N_PSK_HMAC_SHA384:     psk->hmac_alg = S2N_HMAC_SHA384; break;
        default:
            S2N_ERROR(S2N_ERR_HMAC_INVALID_ALGORITHM);
    }
    return S2N_SUCCESS;
}

int s2n_connection_append_psk(struct s2n_connection *conn, struct s2n_psk *input_psk)
{
    notnull_check(conn);
    notnull_check(input_psk);
    struct s2n_array *psk_list = &conn->psk_params.psk_list;
    
    /* Check for duplicate identities */
    for (uint32_t j = 0; j < psk_list->len; j++) {
        struct s2n_psk *existing_psk = NULL;
        GUARD_AS_POSIX(s2n_array_get(psk_list, j, (void**) &existing_psk));
        notnull_check(existing_psk);

        bool duplicate = existing_psk->identity.size == input_psk->identity.size
                && memcmp(existing_psk->identity.data, input_psk->identity.data, existing_psk->identity.size) == 0;
        ENSURE_POSIX(!duplicate, S2N_ERR_DUPLICATE_PSK_IDENTITIES);
    }

    /* Verify the PSK list will fit in the ClientHello pre_shared_key extension */
    if (conn->mode == S2N_CLIENT) {
        uint32_t list_size = 0;
        GUARD_AS_POSIX(s2n_psk_parameters_offered_psks_size(&conn->psk_params, &list_size));

        uint32_t psk_size = 0;
        GUARD_AS_POSIX(s2n_psk_offered_psk_size(input_psk, &psk_size));

        ENSURE_POSIX(list_size + psk_size + S2N_EXTENSION_HEADER_LENGTH <= UINT16_MAX, S2N_ERR_OFFERED_PSKS_TOO_LONG);
    }

    DEFER_CLEANUP(struct s2n_psk new_psk = { 0 }, s2n_psk_wipe);
    ENSURE_POSIX(s2n_result_is_ok(s2n_psk_clone(&new_psk, input_psk)), S2N_ERR_INVALID_ARGUMENT);
    GUARD_AS_POSIX(s2n_array_insert_and_copy(psk_list, psk_list->len, &new_psk));

    ZERO_TO_DISABLE_DEFER_CLEANUP(new_psk);
    return S2N_SUCCESS;
}
