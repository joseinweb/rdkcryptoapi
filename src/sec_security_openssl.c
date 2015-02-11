
/**
 * Copyright 2014 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sec_security_openssl.h"
#include "sec_security_utils.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/err.h>
#include <openssl/aes.h>

#define CHECK_HANDLE(handle) \
    if (NULL == handle) { \
        SEC_LOG_ERROR("Invalid handle"); \
        return SEC_RESULT_INVALID_HANDLE; \
    }

static SecOpenSSL_CustomProcessKeyContainer g_sec_openssl_cpkc = NULL;

void SecOpenssl_RegisterCustomProcessKeyContainer(SecOpenSSL_CustomProcessKeyContainer func)
{
    g_sec_openssl_cpkc = func;
}

int SecOpenSSL_DisablePassphrasePrompt(char *buf, int size, int rwflag, void *u)
{
    return 0;
}

Sec_Result _Sec_SignCertificateData(Sec_ProcessorHandle *proc,
        _Sec_CertificateData *cert_store)
{
    SEC_SIZE macSize;

    CHECK_HANDLE(proc);

    if (SEC_RESULT_SUCCESS != SecMac_SingleInputId(proc, SEC_MACALGORITHM_HMAC_SHA256, SEC_OBJECTID_CERTSTORE_KEY,
            cert_store->cert, cert_store->cert_len, cert_store->mac, &macSize))
    {
        SEC_LOG_ERROR("SecMac_SingleInputId failed");
        return SEC_RESULT_FAILURE;
    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result _Sec_ValidateCertificateData(Sec_ProcessorHandle *proc,
        _Sec_CertificateData *cert_store)
{
    SEC_BYTE macBuffer[SEC_MAC_MAX_LEN];
    SEC_SIZE macSize = 0;

    CHECK_HANDLE(proc);

    if (SEC_RESULT_SUCCESS != SecMac_SingleInputId(proc, SEC_MACALGORITHM_HMAC_SHA256, SEC_OBJECTID_CERTSTORE_KEY,
            cert_store->cert, cert_store->cert_len, macBuffer, &macSize))
    {
        SEC_LOG_ERROR("SecMac_SingleInputId failed");
        return SEC_RESULT_FAILURE;
    }

    if (Sec_Memcmp(macBuffer, cert_store->mac, macSize) != 0)
    {
        SEC_LOG_ERROR("Certificate mac does not match the expected value");
        return SEC_RESULT_FAILURE;
    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result _Sec_SymetricFromKeyHandle(Sec_KeyHandle *key, SEC_BYTE *out_key, SEC_SIZE out_key_len)
{
    SEC_BYTE key_data[SEC_KEYCONTAINER_MAX_LEN];
    SecUtils_KeyStoreHeader keystore_header;
    Sec_Result res = SEC_RESULT_FAILURE;
    SecOpenSSL_DerivedInputs *inputs = NULL;
    SEC_BYTE ladder_1[SEC_SYMETRIC_KEY_MAX_LEN];

    if (!SecKey_IsSymetric(key->key_data.info.key_type))
    {
        SEC_LOG_ERROR("Not a symetric key");
        goto done;
    }

    if (out_key_len != SecKey_GetKeyLen(key))
    {
        SEC_LOG_ERROR("invalid out_key_len");
        goto done;
    }

    if (key->key_data.info.kc_type != SEC_KEYCONTAINER_STORE)
    {
        SEC_LOG_ERROR("Only key store keys are supported on this platform");
        goto done;
    }

    if (SEC_RESULT_SUCCESS != SecStore_RetrieveData(key->proc, SEC_FALSE,
            &keystore_header, sizeof(keystore_header),
            key_data, sizeof(key_data), &key->key_data.kc.store, key->key_data.kc_len))
    {
        SEC_LOG_ERROR("SecStore_RetrieveData failed");
        goto done;
    }

    if (keystore_header.inner_kc_type == SEC_OPENSSL_KEYCONTAINER_DERIVED)
    {
        if (sizeof(SecOpenSSL_DerivedInputs) != SecStore_GetDataLen(key->key_data.kc.buffer))
        {
            SEC_LOG_ERROR("Invalid key length in the store");
            goto done;
        }

        if (out_key_len != SEC_AES_BLOCK_SIZE)
        {
            SEC_LOG_ERROR("derived key can only be the size of the AES block");
            goto done;
        }

        inputs = (SecOpenSSL_DerivedInputs *) key_data;

        /* here we do the derivation in clear.  On a secure chip, this will be done in HW,
           and the resulting key should not be exposed to the host */
        AES_KEY aes_key;
        if (0 != AES_set_encrypt_key(key->proc->root_key, sizeof(key->proc->root_key)*8, &aes_key))
        {
            SEC_LOG_ERROR("AES_set_encrypt_key failed");
            goto done;
        }
        AES_encrypt(inputs->input1, ladder_1, &aes_key);

        if (0 != AES_set_encrypt_key(ladder_1, SEC_AES_BLOCK_SIZE*8, &aes_key))
        {
            SEC_LOG_ERROR("AES_set_encrypt_key failed");
            goto done;
        }
        AES_encrypt(inputs->input2, out_key, &aes_key);
    }
    else
    {
        if (out_key_len != SecStore_GetDataLen(key->key_data.kc.buffer))
        {
            SEC_LOG_ERROR("Invalid key length in the store");
            goto done;
        }

        memcpy(out_key, key_data, out_key_len);
    }

    res = SEC_RESULT_SUCCESS;

done:
    Sec_Memset(key_data, 0, sizeof(key_data));
    Sec_Memset(ladder_1, 0, sizeof(ladder_1));

    return res;
}

RSA *_Sec_RSAFromKeyHandle(Sec_KeyHandle *key)
{
    SecUtils_KeyStoreHeader keystore_header;
    SEC_BYTE key_data[SEC_KEYCONTAINER_MAX_LEN];
    RSA *rsa = NULL;

    if (!SecKey_IsRsa(key->key_data.info.key_type))
    {
        SEC_LOG_ERROR("Not an RSA key");
        goto done;
    }

    /* here the key is loaded in clear.  On a secure processor, the loading
       should be done in a secure manner with the key never being exposed to
       the host processor. */
    if (key->key_data.info.kc_type != SEC_KEYCONTAINER_STORE)
    {
        SEC_LOG_ERROR("Only key store keys are supported on this platform");
        goto done;
    }

    if (SEC_RESULT_SUCCESS != SecStore_RetrieveData(key->proc, SEC_FALSE,
            &keystore_header, sizeof(keystore_header),
            key_data, sizeof(key_data), &key->key_data.kc.store, key->key_data.kc_len))
    {
        SEC_LOG_ERROR("SecStore_RetrieveData failed");
        goto done;
    }

    switch (key->key_data.info.key_type)
    {
        case SEC_KEYTYPE_RSA_1024:
        case SEC_KEYTYPE_RSA_2048:
            rsa = SecUtils_RSAFromPrivBinary((Sec_RSARawPrivateKey*) key_data);
            if (rsa == NULL)
            {
                SEC_LOG_ERROR("SecUtils_RSAFromPrivBinary failed");
                goto done;
            }
            break;

        case SEC_KEYTYPE_RSA_1024_PUBLIC:
        case SEC_KEYTYPE_RSA_2048_PUBLIC:
            rsa = SecUtils_RSAFromPubBinary((Sec_RSARawPublicKey*) key_data);
            if (rsa == NULL)
            {
                SEC_LOG_ERROR("SecUtils_RSAFromPubBinary failed");
                goto done;
            }
            break;

        default:
            SEC_LOG_ERROR("Not an RSA key");
            break;
    }

done:
    Sec_Memset(key_data, 0, sizeof(key_data));
    return rsa;
}

void _Sec_FindRAMKeyData(Sec_ProcessorHandle* secProcHandle, SEC_OBJECTID object_id,
        _Sec_RAMKeyData **data, _Sec_RAMKeyData **parent)
{
    *parent = NULL;
    *data = secProcHandle->ram_keys;

    while ((*data) != NULL )
    {
        if (object_id == (*data)->object_id)
            return;

        *parent = (*data);
        *data = (*data)->next;
    }

    *parent = NULL;
}

void _Sec_FindRAMBundleData(Sec_ProcessorHandle* secProcHandle, SEC_OBJECTID object_id,
        _Sec_RAMBundleData **data, _Sec_RAMBundleData **parent)
{
    *parent = NULL;
    *data = secProcHandle->ram_bundles;

    while ((*data) != NULL)
    {
        if (object_id == (*data)->object_id)
            return;

        *parent = (*data);
        *data = (*data)->next;
    }

    *parent = NULL;
}

void _Sec_FindRAMCertificateData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, _Sec_RAMCertificateData **data,
        _Sec_RAMCertificateData **parent)
{
    *parent = NULL;
    *data = secProcHandle->ram_certs;

    while ((*data) != NULL )
    {
        if (object_id == (*data)->object_id)
            return;

        *parent = (*data);
        *data = (*data)->next;
    }

    *parent = NULL;
}

Sec_Result SecOpenSSL_ProcessKeyContainer(Sec_ProcessorHandle *proc,
        _Sec_KeyData *key_data, Sec_KeyContainer data_type, void *data,
        SEC_SIZE data_len, SEC_OBJECTID objectId)
{
    BIO *bio = NULL;
    RSA *rsa = NULL;
    EVP_PKEY *evp_key = NULL;
    Sec_RSARawPrivateKey rsaPrivKey;
    Sec_RSARawPublicKey rsaPubKey;
    SecUtils_KeyStoreHeader keystore_header;
    const unsigned char *p = (unsigned char*)data;
    PKCS8_PRIV_KEY_INFO *p8;

    memset(key_data, 0, sizeof(_Sec_KeyData));

    if (objectId == SEC_OBJECTID_INVALID)
    {
        SEC_LOG_ERROR("Cannot provision object with SEC_OBJECTID_INVALID");
        return SEC_RESULT_FAILURE;
    }

    if (data_len > SEC_KEYCONTAINER_MAX_LEN)
    {
        SEC_LOG_ERROR("key data is too long");
        return SEC_RESULT_FAILURE;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_AES_128)
    {
        if (data_len != 16)
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_AES_128;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_AES_256)
    {
        if (data_len != 32)
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_AES_256;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_HMAC_128)
    {
        if (data_len != 16)
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_HMAC_128;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_HMAC_160)
    {
        if (data_len != 20)
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_HMAC_160;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_HMAC_256)
    {
        if (data_len != 32)
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_HMAC_256;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_RAW_RSA_1024
            || data_type == SEC_KEYCONTAINER_RAW_RSA_2048)
    {
        if (data_len != sizeof(Sec_RSARawPrivateKey))
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_RAW_RSA_1024) ?
                        SEC_KEYTYPE_RSA_1024 :
                        SEC_KEYTYPE_RSA_2048;

        /* validate the key */
        rsa = SecUtils_RSAFromPrivBinary((Sec_RSARawPrivateKey *) data);
        if (rsa == NULL
                || (SEC_SIZE) RSA_size(rsa) != SecKey_GetKeyLenForKeyType(key_data->info.key_type))
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        SEC_RSA_FREE(rsa);

        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_DER_RSA_1024
            || data_type == SEC_KEYCONTAINER_DER_RSA_2048)
    {
        p = (unsigned char*)data;

        p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, data_len);
        if (p8 != NULL)
        {
            evp_key = EVP_PKCS82PKEY(p8);
            PKCS8_PRIV_KEY_INFO_free(p8);
        }
        else
        {
            evp_key = d2i_AutoPrivateKey(NULL, &p, data_len);
        }

        /*
        evp_key = d2i_AutoPrivateKey(&evp_key, &p, data_len);
        */
        if (evp_key == NULL)
        {
            SEC_LOG_ERROR("d2i_AutoPrivateKey failed");
            return SEC_RESULT_INVALID_PARAMETERS;
        }
        rsa = EVP_PKEY_get1_RSA(evp_key);
        SEC_EVPPKEY_FREE(evp_key);

        if (rsa == NULL)
        {
            SEC_LOG_ERROR("EVP_PKEY_get1_RSA failed");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        /* set key type */
        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_DER_RSA_1024) ?
                        SEC_KEYTYPE_RSA_1024 :
                        SEC_KEYTYPE_RSA_2048;


        SecUtils_RSAToPrivBinary(rsa, &rsaPrivKey);
        SEC_RSA_FREE(rsa);

        return SecOpenSSL_ProcessKeyContainer(proc, key_data,
                data_type == SEC_KEYCONTAINER_DER_RSA_1024 ?
                        SEC_KEYCONTAINER_RAW_RSA_1024 :
                        SEC_KEYCONTAINER_RAW_RSA_2048, &rsaPrivKey,
                sizeof(rsaPrivKey), objectId);
    }

    if (data_type == SEC_KEYCONTAINER_DER_RSA_1024_PUBLIC
            || data_type == SEC_KEYCONTAINER_DER_RSA_2048_PUBLIC)
    {
        p = (unsigned char*)data;
        rsa = d2i_RSAPublicKey(&rsa, &p, data_len);

        if (!rsa)
        {
            p = (unsigned char*)data;
            rsa = d2i_RSA_PUBKEY(&rsa, &p, data_len);
        }

        if (!rsa)
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_DER_RSA_1024_PUBLIC) ?
                        SEC_KEYTYPE_RSA_1024_PUBLIC :
                        SEC_KEYTYPE_RSA_2048_PUBLIC;

        /* validate key */
        if (rsa == NULL || (SEC_SIZE) RSA_size(rsa) != SecKey_GetKeyLenForKeyType(key_data->info.key_type))
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        SecUtils_RSAToPubBinary(rsa, &rsaPubKey);
        SEC_RSA_FREE(rsa);

        return SecOpenSSL_ProcessKeyContainer(proc, key_data,
                data_type == SEC_KEYCONTAINER_DER_RSA_1024_PUBLIC ?
                        SEC_KEYCONTAINER_RAW_RSA_1024_PUBLIC :
                        SEC_KEYCONTAINER_RAW_RSA_2048_PUBLIC, &rsaPubKey,
                sizeof(rsaPubKey), objectId);
    }

    if (data_type == SEC_KEYCONTAINER_RAW_RSA_1024_PUBLIC
            || data_type == SEC_KEYCONTAINER_RAW_RSA_2048_PUBLIC)
    {
        if (data_len != sizeof(Sec_RSARawPublicKey))
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_RAW_RSA_1024_PUBLIC) ?
                        SEC_KEYTYPE_RSA_1024_PUBLIC :
                        SEC_KEYTYPE_RSA_2048_PUBLIC;

        /* validate the key */
        rsa = SecUtils_RSAFromPubBinary((Sec_RSARawPublicKey *) data);
        if (rsa == NULL
                || (SEC_SIZE) RSA_size(rsa) != SecKey_GetKeyLenForKeyType(key_data->info.key_type))
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        SEC_RSA_FREE(rsa);

        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_PEM_RSA_1024
            || data_type == SEC_KEYCONTAINER_PEM_RSA_2048)
    {
        bio = BIO_new_mem_buf(data, data_len);
        rsa = PEM_read_bio_RSAPrivateKey(bio, &rsa,
                SecOpenSSL_DisablePassphrasePrompt, NULL);
        SEC_BIO_FREE(bio);
        bio = NULL;

        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_PEM_RSA_1024) ?
                        SEC_KEYTYPE_RSA_1024 :
                        SEC_KEYTYPE_RSA_2048;

        /* validate key */
        if (rsa == NULL
                || (SEC_SIZE) RSA_size(rsa)
                        != SecKey_GetKeyLenForKeyType(key_data->info.key_type))
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        SecUtils_RSAToPrivBinary(rsa, &rsaPrivKey);
        SEC_RSA_FREE(rsa);

        return SecOpenSSL_ProcessKeyContainer(proc, key_data,
                data_type == SEC_KEYCONTAINER_PEM_RSA_1024 ?
                        SEC_KEYCONTAINER_RAW_RSA_1024 :
                        SEC_KEYCONTAINER_RAW_RSA_2048, &rsaPrivKey,
                sizeof(rsaPrivKey), objectId);
    }

    if (data_type == SEC_KEYCONTAINER_PEM_RSA_1024_PUBLIC
            || data_type == SEC_KEYCONTAINER_PEM_RSA_2048_PUBLIC)
    {
        bio = BIO_new_mem_buf(data, data_len);
        rsa = PEM_read_bio_RSA_PUBKEY(bio, &rsa, SecOpenSSL_DisablePassphrasePrompt,
                NULL);
        SEC_BIO_FREE(bio);
        bio = NULL;

        key_data->info.key_type =
                (data_type == SEC_KEYCONTAINER_PEM_RSA_1024_PUBLIC) ?
                        SEC_KEYTYPE_RSA_1024_PUBLIC :
                        SEC_KEYTYPE_RSA_2048_PUBLIC;

        /* validate key */
        if (rsa == NULL || (SEC_SIZE) RSA_size(rsa) != SecKey_GetKeyLenForKeyType(key_data->info.key_type))
        {
            SEC_RSA_FREE(rsa);
            SEC_LOG_ERROR("Invalid RSA key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        SecUtils_RSAToPubBinary(rsa, &rsaPubKey);
        SEC_RSA_FREE(rsa);

        return SecOpenSSL_ProcessKeyContainer(proc, key_data,
                data_type == SEC_KEYCONTAINER_PEM_RSA_1024_PUBLIC ?
                        SEC_KEYCONTAINER_RAW_RSA_1024_PUBLIC :
                        SEC_KEYCONTAINER_RAW_RSA_2048_PUBLIC, &rsaPubKey,
                sizeof(rsaPubKey), objectId);
    }

    if (data_type == SEC_OPENSSL_KEYCONTAINER_DERIVED)
    {
        if (data_len != sizeof(SecOpenSSL_DerivedInputs))
        {
            SEC_LOG_ERROR("Invalid key container length");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        key_data->info.key_type = SEC_KEYTYPE_AES_128;
        goto store_data;
    }

    if (data_type == SEC_KEYCONTAINER_STORE)
    {
        if (SecStore_GetStoreLen(data) != data_len)
        {
            SEC_LOG_ERROR("Secure store length does not match the expected one");
            return SEC_RESULT_FAILURE;
        }

        /* validate the store */
        if (SEC_RESULT_SUCCESS != SecUtils_ValidateKeyStore(proc, SEC_FALSE, data, data_len))
        {
            SEC_LOG_ERROR("SecUtils_ValidateKeyStore failed");
            return SEC_RESULT_FAILURE;
        }

        memcpy(&key_data->kc, data, data_len);
        key_data->info.kc_type = SEC_KEYCONTAINER_STORE;
        key_data->kc_len = data_len;

        return SEC_RESULT_SUCCESS;
    }

    if (g_sec_openssl_cpkc != NULL)
    {
        return g_sec_openssl_cpkc(proc, key_data, data_type, data, data_len, objectId);
    }

    SEC_LOG_ERROR("Unimplemented key container type");
    return SEC_RESULT_UNIMPLEMENTED_FEATURE;

store_data:
    if (SEC_RESULT_SUCCESS != SecUtils_FillKeyStoreUserHeader(proc, &keystore_header, data_type))
    {
        SEC_LOG_ERROR("SecUtils_FillKeyStoreUserHeader failed");
        return SEC_RESULT_FAILURE;
    }

    /* encrypt store */
    if (SEC_RESULT_SUCCESS != SecStore_StoreData(proc, SEC_TRUE, SEC_TRUE,
            (SEC_BYTE *) SEC_UTILS_KEYSTORE_MAGIC, &keystore_header, sizeof(keystore_header),
            data, data_len, key_data->kc.buffer, sizeof(key_data->kc.buffer)))
    {
        SEC_LOG_ERROR("SecStore_StoreData failed");
        return SEC_RESULT_FAILURE;
    }

    key_data->info.kc_type = SEC_KEYCONTAINER_STORE;
    key_data->kc_len = SecStore_GetStoreLen(key_data->kc.buffer);

    return SEC_RESULT_SUCCESS;
}

Sec_Result _Sec_ProcessCertificateContainer(Sec_ProcessorHandle *proc,
        _Sec_CertificateData *cert_data, Sec_CertificateContainer data_type,
        void *data, SEC_SIZE data_len)
{
    BIO *bio = NULL;
    X509 *x509 = NULL;

    if (data_type == SEC_CERTIFICATECONTAINER_X509_DER)
    {
        bio = BIO_new_mem_buf(data, data_len);
        x509 = d2i_X509_bio(bio, NULL );
        SEC_BIO_FREE(bio);
        bio = NULL;

        if (x509 == NULL )
        {
            SEC_X509_FREE(x509);
            SEC_LOG_ERROR("Invalid X509 key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        memset(cert_data, 0, sizeof(_Sec_CertificateData));
        cert_data->cert_len = SecUtils_X509ToDerLen(x509, cert_data->cert, sizeof(cert_data->cert));
        if (cert_data->cert_len == 0)
        {
            SEC_X509_FREE(x509);
            SEC_LOG_ERROR("Certificate is too large");
            return SEC_RESULT_INVALID_PARAMETERS;
        }
        SEC_X509_FREE(x509);
        return _Sec_SignCertificateData(proc, cert_data);
    }

    if (data_type == SEC_CERTIFICATECONTAINER_X509_PEM)
    {
        bio = BIO_new_mem_buf(data, data_len);
        x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL );
        SEC_BIO_FREE(bio);
        bio = NULL;

        if (x509 == NULL )
        {
            SEC_X509_FREE(x509);
            SEC_LOG_ERROR("Invalid X509 key container");
            return SEC_RESULT_INVALID_PARAMETERS;
        }

        memset(cert_data, 0, sizeof(_Sec_CertificateData));
        cert_data->cert_len = SecUtils_X509ToDerLen(x509, cert_data->cert, sizeof(cert_data->cert));
        if (cert_data->cert_len == 0)
        {
            SEC_X509_FREE(x509);
            SEC_LOG_ERROR("Certificate is too large");
            return SEC_RESULT_INVALID_PARAMETERS;
        }
        SEC_X509_FREE(x509);
        return _Sec_SignCertificateData(proc, cert_data);
    }

    SEC_LOG_ERROR("Unimplemented certificate container type");
    return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result _Sec_RetrieveBundleData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc *location, _Sec_BundleData *bundleData)
{
    char file_name_bundle[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMBundleData *ram_bundle = NULL;
    _Sec_RAMBundleData *ram_bundle_parent = NULL;

    CHECK_HANDLE(secProcHandle);

    /* check in RAM */
    _Sec_FindRAMBundleData(secProcHandle, object_id, &ram_bundle, &ram_bundle_parent);
    if (ram_bundle != NULL)
    {
        memcpy(bundleData, &(ram_bundle->bundle_data), sizeof(_Sec_BundleData));
        *location = SEC_STORAGELOC_RAM;
        return SEC_RESULT_SUCCESS;
    }

    /* check in file system */
    snprintf(file_name_bundle, sizeof(file_name_bundle), "%s" SEC_BUNDLE_FILENAME_PATTERN,
            secProcHandle->bundlestorage_file_dir, object_id);
    if (SecUtils_FileExists(file_name_bundle))
    {
        if (SecUtils_ReadFile(file_name_bundle, bundleData->bundle,
                sizeof(bundleData->bundle), &bundleData->bundle_len) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not read one of the bundle files");
            return SEC_RESULT_FAILURE;
        }

        *location = SEC_STORAGELOC_FILE;

        return SEC_RESULT_SUCCESS;
    }

    /* check OEM provisioned location */
#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif

    return SEC_RESULT_NO_SUCH_ITEM;
}

Sec_Result _Sec_RetrieveKeyData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc *location, _Sec_KeyData *keyData)
{
    char file_name_key[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMKeyData *ram_key = NULL;
    _Sec_RAMKeyData *ram_key_parent = NULL;
    SEC_SIZE data_read;

    CHECK_HANDLE(secProcHandle);

    /* check in RAM */
    _Sec_FindRAMKeyData(secProcHandle, object_id, &ram_key, &ram_key_parent);
    if (ram_key != NULL )
    {
        memcpy(keyData, &(ram_key->key_data), sizeof(_Sec_KeyData));
        *location = SEC_STORAGELOC_RAM;
        return SEC_RESULT_SUCCESS;
    }

    /* check in file system */
    snprintf(file_name_key, sizeof(file_name_key), "%s" SEC_KEY_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
            object_id);
    snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_KEYINFO_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
            object_id);
    if (SecUtils_FileExists(file_name_key) && SecUtils_FileExists(file_name_info))
    {
        if (SecUtils_ReadFile(file_name_key, keyData->kc.buffer, sizeof(keyData->kc), &keyData->kc_len) != SEC_RESULT_SUCCESS
                || SecUtils_ReadFile(file_name_info, &keyData->info, sizeof(keyData->info), &data_read) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not read one of the key files");
            return SEC_RESULT_FAILURE;
        }

        if (data_read != sizeof(keyData->info))
        {
            SEC_LOG_ERROR("File is not of the correct size");
            return SEC_RESULT_FAILURE;
        }

        *location = SEC_STORAGELOC_FILE;

        return SEC_RESULT_SUCCESS;
    }

#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif

    return SEC_RESULT_NO_SUCH_ITEM;
}

Sec_Result _Sec_RetrieveCertificateData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc *location,
        _Sec_CertificateData *certData)
{
    char file_name_cert[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMCertificateData *ram_cert = NULL;
    _Sec_RAMCertificateData *ram_cert_parent = NULL;
    SEC_SIZE data_read;

    CHECK_HANDLE(secProcHandle);

    /* check in RAM */
    _Sec_FindRAMCertificateData(secProcHandle, object_id, &ram_cert,
            &ram_cert_parent);
    if (ram_cert != NULL )
    {
        memcpy(certData, &(ram_cert->cert_data), sizeof(_Sec_CertificateData));
        *location = SEC_STORAGELOC_RAM;
        return SEC_RESULT_SUCCESS;
    }

    /* check in file system */
    snprintf(file_name_cert, sizeof(file_name_cert), "%s" SEC_CERT_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
            object_id);
    snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_CERTINFO_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
            object_id);
    if (SecUtils_FileExists(file_name_cert) && SecUtils_FileExists(file_name_info))
    {
        if (SecUtils_ReadFile(file_name_cert, certData->cert, sizeof(certData->cert), &certData->cert_len) != SEC_RESULT_SUCCESS
                || SecUtils_ReadFile(file_name_info, certData->mac, sizeof(certData->mac), &data_read) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not read one of the certificate files");
            return SEC_RESULT_FAILURE;
        }

        if (data_read != sizeof(certData->mac))
        {
            SEC_LOG_ERROR("File is not of the correct size");
            return SEC_RESULT_FAILURE;
        }

        *location = SEC_STORAGELOC_FILE;

        return SEC_RESULT_SUCCESS;
    }

#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif

    return SEC_RESULT_NO_SUCH_ITEM;
}

Sec_Result _Sec_StoreBundleData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location, _Sec_BundleData *bundleData)
{
    _Sec_RAMBundleData *ram_bundle;
    char file_name_bundle[SEC_MAX_FILE_PATH_LEN];

    if (location == SEC_STORAGELOC_RAM)
    {
        SecBundle_Delete(secProcHandle, object_id);

        ram_bundle = calloc(1, sizeof(_Sec_RAMBundleData));
        if (NULL == ram_bundle)
        {
            SEC_LOG_ERROR("malloc failed");
            return SEC_RESULT_FAILURE;
        }
        ram_bundle->object_id = object_id;
        memcpy(&(ram_bundle->bundle_data), bundleData, sizeof(_Sec_BundleData));
        ram_bundle->next = secProcHandle->ram_bundles;
        secProcHandle->ram_bundles = ram_bundle;

        return SEC_RESULT_SUCCESS;
    }

    if (location == SEC_STORAGELOC_FILE)
    {
        SecBundle_Delete(secProcHandle, object_id);

        snprintf(file_name_bundle, sizeof(file_name_bundle), "%s" SEC_BUNDLE_FILENAME_PATTERN,
                secProcHandle->bundlestorage_file_dir, object_id);

        if (SecUtils_WriteFile(file_name_bundle, bundleData->bundle,
                bundleData->bundle_len) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not write one of the bundle files");
            SecUtils_RmFile(file_name_bundle);
            return SEC_RESULT_FAILURE;
        }

        return SEC_RESULT_SUCCESS;
    }

    SEC_LOG_ERROR("Unimplemented location type");
    return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result _Sec_StoreKeyData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location, _Sec_KeyData *keyData)
{
    _Sec_RAMKeyData *ram_key;
    char file_name_key[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];

    if (location == SEC_STORAGELOC_RAM
            || location == SEC_STORAGELOC_RAM_SOFT_WRAPPED)
    {
        SecKey_Delete(secProcHandle, object_id);

        ram_key = calloc(1, sizeof(_Sec_RAMKeyData));
        if (NULL == ram_key)
        {
            SEC_LOG_ERROR("malloc failed");
            return SEC_RESULT_FAILURE;
        }
        ram_key->object_id = object_id;
        memcpy(&(ram_key->key_data), keyData, sizeof(_Sec_KeyData));
        ram_key->next = secProcHandle->ram_keys;
        secProcHandle->ram_keys = ram_key;

        return SEC_RESULT_SUCCESS;
    }
    else if (location == SEC_STORAGELOC_FILE
            || location == SEC_STORAGELOC_FILE_SOFT_WRAPPED)
    {
        SecKey_Delete(secProcHandle, object_id);

        snprintf(file_name_key, sizeof(file_name_key), "%s" SEC_KEY_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
                object_id);
        snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_KEYINFO_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
                object_id);

        if (SecUtils_WriteFile(file_name_key, keyData->kc.buffer, keyData->kc_len) != SEC_RESULT_SUCCESS
                || SecUtils_WriteFile(file_name_info, &keyData->info, sizeof(keyData->info)) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not write one of the key files");
            SecUtils_RmFile(file_name_key);
            SecUtils_RmFile(file_name_info);
            return SEC_RESULT_FAILURE;
        }

        return SEC_RESULT_SUCCESS;
    }
    else if (location == SEC_STORAGELOC_OEM)
    {
        SEC_LOG_ERROR("Cannot store keys in SEC_STORAGELOC_OEM on this platform");
        return SEC_RESULT_FAILURE;
    }

    SEC_LOG_ERROR("Unimplemented location type");
    return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result _Sec_StoreCertificateData(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location,
        _Sec_CertificateData *certData)
{
    _Sec_RAMCertificateData *ram_cert;
    char file_name_cert[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];

    if (location == SEC_STORAGELOC_RAM)
    {
        SecCertificate_Delete(secProcHandle, object_id);

        ram_cert = calloc(1, sizeof(_Sec_RAMCertificateData));
        if (NULL == ram_cert)
        {
            SEC_LOG_ERROR("malloc failed");
            return SEC_RESULT_FAILURE;
        }
        ram_cert->object_id = object_id;
        memcpy(&(ram_cert->cert_data), certData, sizeof(_Sec_CertificateData));
        ram_cert->next = secProcHandle->ram_certs;
        secProcHandle->ram_certs = ram_cert;

        return SEC_RESULT_SUCCESS;
    }
    else if (location == SEC_STORAGELOC_FILE)
    {
        SecCertificate_Delete(secProcHandle, object_id);

        snprintf(file_name_cert, sizeof(file_name_cert), "%s" SEC_CERT_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
                object_id);
        snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_CERTINFO_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
                object_id);

        if (SecUtils_WriteFile(file_name_cert, certData->cert, certData->cert_len) != SEC_RESULT_SUCCESS
                || SecUtils_WriteFile(file_name_info, certData->mac, sizeof(certData->mac)) != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("Could not write one of the cert files");
            SecUtils_RmFile(file_name_cert);
            SecUtils_RmFile(file_name_info);
        }

        return SEC_RESULT_SUCCESS;
    }
    else if (location == SEC_STORAGELOC_OEM)
    {
        SEC_LOG_ERROR("Cannot store cert files in SEC_STORAGELOC_OEM on this platform");
        return SEC_RESULT_FAILURE;
    }

    SEC_LOG_ERROR("Unimplemented location type");
    return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result _Sec_SetStorageDir(const char *provided_dir, const char *default_dir,
        char *output_dir)
{
    const char * dir_to_use;
    size_t len;

    if (provided_dir == NULL || strlen(provided_dir) == 0)
        dir_to_use = default_dir;
    else
        dir_to_use = provided_dir;

    len = strlen(dir_to_use);
    if (len >= (SEC_MAX_FILE_PATH_LEN - 2))
    {
        SEC_LOG_ERROR("directory name length is too long");
        return SEC_RESULT_FAILURE;
    }

    snprintf(output_dir, SEC_MAX_FILE_PATH_LEN, "%s", dir_to_use);

    if (output_dir[len - 1] != '/' && output_dir[len - 1] != '\\')
    {
        output_dir[len] = '/';
        output_dir[len + 1] = '\0';
    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result _Sec_ProvisionBaseKey(Sec_ProcessorHandle *secProcHandle, SEC_BYTE *nonce)
{
    /* constants */
    const char *inputDerivationStr = "sivSha1";
    Sec_DigestAlgorithm digestAlgorithm = SEC_DIGESTALGORITHM_SHA1;
    const char *cipherAlgorithmStr = "aesEcbNone";
    Sec_CipherAlgorithm cipherAlgorithm = SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING;
    Sec_CipherMode cipherMode = SEC_CIPHERMODE_ENCRYPT;
    Sec_KeyType keyType = SEC_KEYTYPE_AES_128;

    int i;
    SEC_SIZE keySize;
    Sec_Result res = SEC_RESULT_FAILURE;
    SEC_SIZE cipher_output_len;
    SEC_BYTE cipher_output[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_BYTE *cipher_key = secProcHandle->root_key;
    SEC_OBJECTID temp_key_id = SEC_OBJECTID_INVALID;
    SEC_BYTE c1[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_BYTE c2[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_BYTE c3[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_BYTE c4[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_BYTE *c[] = { c1, c2, c3, c4 };

    keySize = SecKey_GetKeyLenForKeyType(keyType);

    res = SecKey_ComputeBaseKeyLadderInputs(secProcHandle, inputDerivationStr, cipherAlgorithmStr,
            nonce, digestAlgorithm, keySize, c1, c2, c3, c4);
    if (res != SEC_RESULT_SUCCESS)
    {
        SEC_LOG_ERROR("SecKey_ComputeBaseKeyLadderInputs failed");
        goto done;
    }

    for (i = 1; i <= 4; i++)
    {
        /* encrypt digest */
        temp_key_id = SEC_OBJECTID_OPENSSL_DERIVE_TMP;

        /* provision temp key */
        res = SecKey_Provision(secProcHandle, temp_key_id, SEC_STORAGELOC_RAM, SEC_KEYCONTAINER_RAW_AES_128, cipher_key, keySize);
        if (SEC_RESULT_SUCCESS != res)
        {
            SEC_LOG_ERROR("SecKey_Provision failed");
            goto done;
        }

        res = SecCipher_SingleInputId(secProcHandle, cipherAlgorithm, cipherMode, temp_key_id, NULL,
                c[i-1], keySize, cipher_output, sizeof(cipher_output), &cipher_output_len);

        /* delete temp key */
        SecKey_Delete(secProcHandle, temp_key_id);

        if (SEC_RESULT_SUCCESS != res)
        {
            SEC_LOG_ERROR("SecCipher_SingleInputId failed");
            goto done;
        }

        cipher_key = cipher_output;
    }

    res = SecKey_Provision(secProcHandle, SEC_OBJECTID_BASE_KEY_AES,
            SEC_STORAGELOC_RAM, SEC_KEYCONTAINER_RAW_AES_128, cipher_key, keySize);

    if (res == SEC_RESULT_SUCCESS)
    {
        res = SecKey_Provision(secProcHandle, SEC_OBJECTID_BASE_KEY_MAC,
                SEC_STORAGELOC_RAM_SOFT_WRAPPED, SEC_KEYCONTAINER_RAW_HMAC_128,
                cipher_key, keySize);
    }

done:
    return res;
}

SEC_BYTE* Sec_NativeMalloc(Sec_ProcessorHandle* secProcHandle, SEC_SIZE length)
{
    return malloc(length);
}

void Sec_NativeFree(Sec_ProcessorHandle* secProcHandle, void *ptr)
{
    if (ptr != NULL)
        free(ptr);
}

Sec_Result SecProcessor_PrintInfo(Sec_ProcessorHandle* secProcHandle)
{
    SEC_BYTE deviceId[SEC_DEVICEID_LEN];

    if (SEC_RESULT_SUCCESS == SecProcessor_GetDeviceId(secProcHandle, deviceId))
    {
        SEC_PRINT("device id: "); Sec_PrintHex(deviceId, SEC_DEVICEID_LEN); SEC_PRINT("\n");
    }
    else
    {
        SEC_PRINT("device id: unknown\n");
    }

    SEC_PRINT("platform: SEC_PLATFORM_OPENSSL\n");
    SEC_PRINT("version: %s\n", SEC_API_VERSION);
    Sec_PrintOpenSSLVersion();

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecProcessor_GetInstance(Sec_ProcessorHandle** secProcHandle,
        Sec_ProcessorInitParams* socInitParams)
{
    const char *otherInfo = "certMacKey" "hmacSha256" "concatKdfSha1";
    const char *nonce = "abcdefghijklmnopqr\0\0";
    SecOpenSSL_DerivedInputs secStoreProcIns;
    SecUtils_KeyStoreHeader keystore_header;
    SEC_BYTE store[SEC_KEYCONTAINER_MAX_LEN];
    *secProcHandle = NULL;

    /* setup openssl stuff */
    Sec_InitOpenSSL();

    /* create handle */
    *secProcHandle = calloc(1, sizeof(Sec_ProcessorHandle));
    if (NULL == *secProcHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }

    /* setup key and cert directories */
    CHECK_EXACT(
            _Sec_SetStorageDir(socInitParams != NULL ? socInitParams->keystorage_file_dir : NULL,
                    SEC_KEYSTORAGE_FILE_DEFAULT_DIR, (*secProcHandle)->keystorage_file_dir),
                    SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecUtils_MkDir((*secProcHandle)->keystorage_file_dir), SEC_RESULT_SUCCESS, error);

    CHECK_EXACT(
            _Sec_SetStorageDir(socInitParams != NULL ? socInitParams->certstorage_file_dir : NULL,
                    SEC_CERTIFICATESTORAGE_FILE_DEFAULT_DIR, (*secProcHandle)->certstorage_file_dir),
                    SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecUtils_MkDir((*secProcHandle)->certstorage_file_dir), SEC_RESULT_SUCCESS, error);

    CHECK_EXACT(
            _Sec_SetStorageDir(socInitParams != NULL ? socInitParams->bundlestorage_file_dir : NULL,
                    SEC_BUNDLESTORAGE_FILE_DEFAULT_DIR, (*secProcHandle)->bundlestorage_file_dir),
                    SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecUtils_MkDir((*secProcHandle)->bundlestorage_file_dir), SEC_RESULT_SUCCESS, error);

    /* device id */
    (*secProcHandle)->device_id[0] = 0x00;
    (*secProcHandle)->device_id[1] = 0x01;
    (*secProcHandle)->device_id[2] = 0x02;
    (*secProcHandle)->device_id[3] = 0x03;
    (*secProcHandle)->device_id[4] = 0x04;
    (*secProcHandle)->device_id[5] = 0x05;
    (*secProcHandle)->device_id[6] = 0x06;
    (*secProcHandle)->device_id[7] = 0x07;

    /* root_key */
    (*secProcHandle)->root_key[0] = 0x00;
    (*secProcHandle)->root_key[1] = 0x01;
    (*secProcHandle)->root_key[2] = 0x02;
    (*secProcHandle)->root_key[3] = 0x03;
    (*secProcHandle)->root_key[4] = 0x04;
    (*secProcHandle)->root_key[5] = 0x05;
    (*secProcHandle)->root_key[6] = 0x06;
    (*secProcHandle)->root_key[7] = 0x07;
    (*secProcHandle)->root_key[8] = 0x08;
    (*secProcHandle)->root_key[9] = 0x09;
    (*secProcHandle)->root_key[10] = 0x0A;
    (*secProcHandle)->root_key[11] = 0x0B;
    (*secProcHandle)->root_key[12] = 0x0C;
    (*secProcHandle)->root_key[13] = 0x0D;
    (*secProcHandle)->root_key[14] = 0x0E;
    (*secProcHandle)->root_key[15] = 0x0F;

    /* generate sec store proc ins */
    CHECK_EXACT(SecStore_GenerateLadderInputs(*secProcHandle, SEC_STORE_AES_LADDER_INPUT,
            NULL,
            (SEC_BYTE*) &secStoreProcIns, sizeof(secStoreProcIns)),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecUtils_FillKeyStoreUserHeader(*secProcHandle, &keystore_header, SEC_OPENSSL_KEYCONTAINER_DERIVED),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecStore_StoreData(*secProcHandle, SEC_FALSE, SEC_FALSE,
            (SEC_BYTE*) SEC_UTILS_KEYSTORE_MAGIC, &keystore_header, sizeof(keystore_header),
            &secStoreProcIns, sizeof(secStoreProcIns), store, sizeof(store)),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecKey_Provision(*secProcHandle, SEC_OBJECTID_STORE_AES_KEY, SEC_STORAGELOC_RAM_SOFT_WRAPPED,
            SEC_KEYCONTAINER_STORE, store, SecStore_GetStoreLen(store)),
            SEC_RESULT_SUCCESS, error);

    CHECK_EXACT(SecStore_GenerateLadderInputs(*secProcHandle, SEC_STORE_MAC_LADDER_INPUT,
            NULL,
            (SEC_BYTE*) &secStoreProcIns, sizeof(secStoreProcIns)),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecUtils_FillKeyStoreUserHeader(*secProcHandle, &keystore_header, SEC_OPENSSL_KEYCONTAINER_DERIVED),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecStore_StoreData(*secProcHandle, SEC_FALSE, SEC_FALSE,
            (SEC_BYTE*) SEC_UTILS_KEYSTORE_MAGIC, &keystore_header, sizeof(keystore_header),
            &secStoreProcIns, sizeof(secStoreProcIns), store, sizeof(store)),
            SEC_RESULT_SUCCESS, error);
    CHECK_EXACT(SecKey_Provision(*secProcHandle, SEC_OBJECTID_STORE_MACKEYGEN_KEY, SEC_STORAGELOC_RAM_SOFT_WRAPPED,
            SEC_KEYCONTAINER_STORE, store, SecStore_GetStoreLen(store)),
            SEC_RESULT_SUCCESS, error);

    /* generate certificate mac key */
    CHECK_EXACT(
            SecKey_Derive_ConcatKDF(*secProcHandle, SEC_OBJECTID_CERTSTORE_KEY, SEC_KEYTYPE_HMAC_256, SEC_STORAGELOC_RAM_SOFT_WRAPPED, SEC_DIGESTALGORITHM_SHA256, (SEC_BYTE *) nonce, (SEC_BYTE *) otherInfo, strlen(otherInfo)),
            SEC_RESULT_SUCCESS, error);

    return SEC_RESULT_SUCCESS;

error:
    if ((*secProcHandle) != NULL )
    {
#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif
        SEC_FREE(*secProcHandle);
        *secProcHandle = NULL;
    }

    return SEC_RESULT_FAILURE;
}

Sec_Result SecProcessor_GetDeviceId(Sec_ProcessorHandle* secProcHandle,
        SEC_BYTE *deviceId)
{
    CHECK_HANDLE(secProcHandle);

    memcpy(deviceId, secProcHandle->device_id,
            sizeof(secProcHandle->device_id));

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecProcessor_Release(Sec_ProcessorHandle *secProcHandle)
{
    if (NULL == secProcHandle)
        return SEC_RESULT_SUCCESS;

    /* release ram keys */
    while (secProcHandle->ram_keys != NULL )
    {
        SecKey_Delete(secProcHandle, secProcHandle->ram_keys->object_id);
    }

    /* release ram bundles */
    while (secProcHandle->ram_bundles != NULL)
    {
        SecBundle_Delete(secProcHandle, secProcHandle->ram_bundles->object_id);
    }

    /* release ram certs */
    while (secProcHandle->ram_certs != NULL )
    {
        SecCertificate_Delete(secProcHandle,
                secProcHandle->ram_certs->object_id);
    }

    ERR_free_strings();

#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif

    SEC_FREE(secProcHandle);

    return SEC_RESULT_SUCCESS;
}

SEC_SIZE SecProcessor_GetKeyLadderMinDepth(Sec_ProcessorHandle* handle, Sec_KeyLadderRoot root)
{
    if (root == SEC_KEYLADDERROOT_UNIQUE) {
        return 2;
    }

    return 0;
}

SEC_SIZE SecProcessor_GetKeyLadderMaxDepth(Sec_ProcessorHandle* handle, Sec_KeyLadderRoot root)
{
    if (root == SEC_KEYLADDERROOT_UNIQUE) {
        return 2;
    }

    return 0;
}

Sec_Result SecCipher_GetInstance(Sec_ProcessorHandle* secProcHandle,
        Sec_CipherAlgorithm algorithm, Sec_CipherMode mode, Sec_KeyHandle* key,
        SEC_BYTE *iv, Sec_CipherHandle** cipherHandle)
{
    Sec_CipherHandle localHandle;
    const EVP_CIPHER *evp_cipher = NULL;
    Sec_Result res = SEC_RESULT_FAILURE;
    SEC_BYTE symetric_key[SEC_SYMETRIC_KEY_MAX_LEN];
    int padding = 0;

    CHECK_HANDLE(secProcHandle);

    memset(&localHandle, 0, sizeof(localHandle));

    if (SEC_RESULT_SUCCESS
            != SecCipher_IsValidKey(key->key_data.info.key_type, algorithm, mode,
                    iv))
    {
        SEC_LOG_ERROR("Invalid key used for specified algorithm");
        goto done;
    }

    switch (algorithm)
    {
        case SEC_CIPHERALGORITHM_AES_CBC_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_PKCS7_PADDING:
        case SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_ECB_PKCS7_PADDING:
            if (algorithm == SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING
                    || algorithm == SEC_CIPHERALGORITHM_AES_ECB_PKCS7_PADDING)
            {
                if (key->key_data.info.key_type == SEC_KEYTYPE_AES_128)
                    evp_cipher = EVP_aes_128_ecb();
                else
                    evp_cipher = EVP_aes_256_ecb();
            }
            else if (algorithm == SEC_CIPHERALGORITHM_AES_CBC_NO_PADDING
                    || algorithm == SEC_CIPHERALGORITHM_AES_CBC_PKCS7_PADDING)
            {
                if (key->key_data.info.key_type == SEC_KEYTYPE_AES_128)
                    evp_cipher = EVP_aes_128_cbc();
                else
                    evp_cipher = EVP_aes_256_cbc();
            }

            EVP_CIPHER_CTX_init(&localHandle.evp_ctx);

            if (1
                    != EVP_CipherInit_ex(&localHandle.evp_ctx, evp_cipher, NULL,
                            NULL, NULL, (mode == SEC_CIPHERMODE_ENCRYPT || mode == SEC_CIPHERMODE_ENCRYPT_NATIVEMEM) ? 1 : 0))
            {
                SEC_LOG_ERROR("EVP_CipherInit failed");
                goto done;
            }

            if (1 != EVP_CIPHER_CTX_set_padding(&localHandle.evp_ctx, padding))
            {
                SEC_LOG_ERROR("EVP_CIPHER_CTX_set_padding failed");
                goto done;
            }

            if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(key, symetric_key, SecKey_GetKeyLen(key)))
            {
                SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
                goto done;
            }

            if (1 != EVP_CipherInit_ex(&localHandle.evp_ctx, NULL, NULL,
                            symetric_key, iv,
                            (mode == SEC_CIPHERMODE_ENCRYPT || mode == SEC_CIPHERMODE_ENCRYPT_NATIVEMEM) ? 1 : 0))
            {
                SEC_LOG_ERROR("EVP_CipherInit failed");
                goto done;
            }

            break;

        case SEC_CIPHERALGORITHM_AES_CTR:
            memset(&localHandle.ctr_ctx, 0, sizeof(localHandle.ctr_ctx));
            memcpy(localHandle.ctr_ctx.ivec, iv, 16);

            if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(key, symetric_key, SecKey_GetKeyLen(key)))
            {
                SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
                goto done;
            }

            if (0 != AES_set_encrypt_key(symetric_key,
                            SecKey_GetKeyLen(key) * 8,
                            &localHandle.ctr_ctx.aes_key))
            {
                SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
                goto done;
            }
            break;

        case SEC_CIPHERALGORITHM_RSA_PKCS1_PADDING:
        case SEC_CIPHERALGORITHM_RSA_OAEP_PADDING:
            /* key is set in the process method */
            break;

        default:
            SEC_LOG_ERROR("Unimplemented cipher algorithm");
            goto done;
    }

    *cipherHandle = calloc(1, sizeof(Sec_CipherHandle));
    if (NULL == *cipherHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        goto done;
    }

    memcpy(*cipherHandle, &localHandle, sizeof(localHandle));
    (*cipherHandle)->algorithm = algorithm;
    (*cipherHandle)->mode = mode;
    (*cipherHandle)->key_handle = key;

    res = SEC_RESULT_SUCCESS;

done:
    Sec_Memset(symetric_key, 0, sizeof(symetric_key));
    return res;
}

Sec_Result SecCipher_ProcessFragmented(Sec_CipherHandle* cipherHandle, SEC_BYTE* input,
        SEC_SIZE inputSize, SEC_BOOL lastInput, SEC_BYTE* output, SEC_SIZE outputSize,
        SEC_SIZE *bytesWritten, SEC_SIZE fragmentOffset, SEC_SIZE fragmentSize, SEC_SIZE fragmentPeriod)
{
    SEC_SIZE lbw;
    SEC_SIZE outputSizeRequired = 0;
    Sec_Result res = SEC_RESULT_FAILURE;

    CHECK_HANDLE(cipherHandle);

    *bytesWritten = 0;

    if (SEC_RESULT_SUCCESS != SecCipher_GetRequiredOutputSizeFragmented(cipherHandle->algorithm,
            cipherHandle->mode, cipherHandle->key_handle->key_data.info.key_type,
            inputSize, &outputSizeRequired, lastInput, fragmentOffset, fragmentSize, fragmentPeriod))
    {
        SEC_LOG_ERROR("SecCipher_GetRequiredOutputSizeFragmented failed");
        goto done;
    }

    if (output == NULL)
    {
        *bytesWritten = outputSizeRequired;
        res = SEC_RESULT_SUCCESS;
        goto done;
    }
    else if (outputSizeRequired > outputSize)
    {
        SEC_LOG_ERROR("output buffer is too small");
        res = SEC_RESULT_INVALID_INPUT_SIZE;
        goto done;
    }

    switch (cipherHandle->algorithm)
    {
        case SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_ECB_PKCS7_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_PKCS7_PADDING:
        case SEC_CIPHERALGORITHM_AES_CTR:
            if (input != output)
            {
                memcpy(output, input, inputSize);
            }
            *bytesWritten = inputSize;

            while (inputSize > 0)
            {
                if (SEC_RESULT_SUCCESS != SecCipher_Process(cipherHandle, output+fragmentOffset, fragmentSize,
                        lastInput && (inputSize == fragmentPeriod), output+fragmentOffset, fragmentSize, &lbw))
                {
                    SEC_LOG_ERROR("SecCipher_Process failed");
                    goto done;
                }
                output += fragmentPeriod;
                inputSize -= fragmentPeriod;
            }
            break;

            /* NEW: other cipher algorithms */
        default:
            SEC_LOG_ERROR("Unimplemented cipher algorithm");
            goto done;
    }

    res = SEC_RESULT_SUCCESS;

done:
    return res;
}

Sec_Result SecCipher_Process(Sec_CipherHandle* cipherHandle, SEC_BYTE* input,
        SEC_SIZE inputSize, SEC_BOOL lastInput, SEC_BYTE* output,
        SEC_SIZE outputSize, SEC_SIZE *bytesWritten)
{
    RSA *rsa;
    int out_len = 0;
    SEC_BYTE aes_pad_vals[SEC_AES_BLOCK_SIZE];
    SEC_BYTE aes_padded_block[SEC_AES_BLOCK_SIZE];
    SEC_BYTE pad_val;
    SEC_SIZE outputSizeNeeded = 0;
    int openssl_res;
    int padding;
    Sec_Result res = SEC_RESULT_FAILURE;

    CHECK_HANDLE(cipherHandle);

    *bytesWritten = 0;

    if (cipherHandle->last != 0)
    {
        SEC_LOG_ERROR("Last block has already been processed");
        return SEC_RESULT_FAILURE;
    }
    cipherHandle->last = lastInput;

    if (SEC_RESULT_SUCCESS != SecCipher_GetRequiredOutputSize(cipherHandle->algorithm,
            cipherHandle->mode, cipherHandle->key_handle->key_data.info.key_type,
            inputSize, &outputSizeNeeded, lastInput))
    {
        SEC_LOG_ERROR("SecCipher_GetRequiredOutputSize failed");
        res = SEC_RESULT_FAILURE;
        goto done;
    }

    if (output == NULL)
    {
        *bytesWritten = outputSizeNeeded;
        res = SEC_RESULT_SUCCESS;
        goto done;
    }
    else if (outputSizeNeeded > outputSize)
    {
        SEC_LOG_ERROR("output buffer is too small");
        res = SEC_RESULT_FAILURE;
        goto done;
    }

    switch (cipherHandle->algorithm)
    {
        case SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_NO_PADDING:
            out_len = 0;
            if (1 != EVP_CipherUpdate(&cipherHandle->evp_ctx, output,
                            &out_len, input, inputSize))
            {
                SEC_LOG_ERROR("EVP_CipherUpdate failed");
                goto done;
            }
            *bytesWritten += out_len;
            out_len = 0;

            if (lastInput
                    && 1 != EVP_CipherFinal_ex(&cipherHandle->evp_ctx,
                                    &output[*bytesWritten], &out_len))
            {
                SEC_LOG_ERROR("EVP_CipherFinal failed");
                goto done;
            }

            *bytesWritten += out_len;
            break;

        case SEC_CIPHERALGORITHM_AES_ECB_PKCS7_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_PKCS7_PADDING:
            out_len = 0;

            /* process all blocks except for the last, partial one */
            if (1 != EVP_CipherUpdate(&cipherHandle->evp_ctx, output,
                            &out_len, input, (inputSize / 16) * 16))
            {
                SEC_LOG_ERROR("EVP_CipherUpdate failed");
                goto done;
            }
            *bytesWritten += out_len;
            out_len = 0;

            if (lastInput && (cipherHandle->mode == SEC_CIPHERMODE_ENCRYPT || cipherHandle->mode == SEC_CIPHERMODE_ENCRYPT_NATIVEMEM))
            {
                /* create padded block */
                SecCipher_PadAESPKCS7Block(input == NULL ? NULL : (input + ((inputSize / 16) * 16)),
                        inputSize % SEC_AES_BLOCK_SIZE, aes_padded_block);

                /* process padded block */
                if (1 != EVP_CipherUpdate(&cipherHandle->evp_ctx,
                                &output[(inputSize / 16) * 16], &out_len,
                                aes_padded_block, SEC_AES_BLOCK_SIZE))
                {
                    SEC_LOG_ERROR("EVP_CipherUpdate failed");
                    goto done;
                }
                *bytesWritten += out_len;
                out_len = 0;

                if (lastInput
                        && 1 != EVP_CipherFinal_ex(&cipherHandle->evp_ctx,
                                        &output[*bytesWritten], &out_len))
                {
                    SEC_LOG_ERROR("EVP_CipherFinal failed");
                    goto done;
                }
                *bytesWritten += out_len;
            }
            else if (lastInput && (cipherHandle->mode == SEC_CIPHERMODE_DECRYPT || cipherHandle->mode == SEC_CIPHERMODE_DECRYPT_NATIVEMEM))
            {
                out_len = 0;
                if (lastInput
                        && 1 != EVP_CipherFinal(&cipherHandle->evp_ctx,
                                        &output[*bytesWritten], &out_len))
                {
                    SEC_LOG_ERROR("EVP_CipherFinal failed");
                    goto done;
                }
                *bytesWritten += out_len;

                /* check padding */
                if (*bytesWritten >= SEC_AES_BLOCK_SIZE) {
                    pad_val = output[*bytesWritten - 1];
                    if (pad_val > SEC_AES_BLOCK_SIZE || pad_val == 0)
                    {
                        SEC_LOG_ERROR("Invalid pad value encountered");
                        return SEC_RESULT_INVALID_PADDING;
                    }

                    memset(aes_pad_vals, pad_val, sizeof(aes_pad_vals));
                    if (Sec_Memcmp(aes_pad_vals, &output[*bytesWritten - pad_val], pad_val) != 0)
                    {
                        SEC_LOG_ERROR("Invalid pad value encountered");
                        return SEC_RESULT_INVALID_PADDING;
                    }

                    /* remove pading values from output */
                    *bytesWritten -= pad_val;
                }
            }
            break;

        case SEC_CIPHERALGORITHM_AES_CTR:
            if (inputSize > 0)
            {
                AES_ctr128_encrypt(input, output, inputSize,
                        &(cipherHandle->ctr_ctx.aes_key),
                        cipherHandle->ctr_ctx.ivec, cipherHandle->ctr_ctx.ecount,
                        &(cipherHandle->ctr_ctx.num));
            }
            *bytesWritten = inputSize;
            break;

        case SEC_CIPHERALGORITHM_RSA_PKCS1_PADDING:
        case SEC_CIPHERALGORITHM_RSA_OAEP_PADDING:
            rsa = _Sec_RSAFromKeyHandle(cipherHandle->key_handle);
            if (NULL == rsa)
            {
                SEC_LOG_ERROR("_Sec_RSAFromKeyHandle failed");
                goto done;
            }

            if (cipherHandle->algorithm
                    == SEC_CIPHERALGORITHM_RSA_PKCS1_PADDING)
            {
                padding = RSA_PKCS1_PADDING;
            }
            else
            {
                padding = RSA_PKCS1_OAEP_PADDING;
            }

            if (cipherHandle->mode == SEC_CIPHERMODE_ENCRYPT || cipherHandle->mode == SEC_CIPHERMODE_ENCRYPT_NATIVEMEM)
            {
                openssl_res = RSA_public_encrypt(inputSize, input, output,
                        rsa, padding);
            }
            else
            {
                openssl_res = RSA_private_decrypt(inputSize, input, output,
                        rsa, padding);
            }

            SEC_RSA_FREE(rsa);

            if (openssl_res < 0)
            {
                SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
                goto done;
            }

            *bytesWritten = openssl_res;
            break;

        default:
            SEC_LOG_ERROR("Unimplemented cipher algorithm");
            goto done;
    }

    res = SEC_RESULT_SUCCESS;

done:
    return res;
}

Sec_Result SecCipher_Release(Sec_CipherHandle* cipherHandle)
{
    CHECK_HANDLE(cipherHandle);

    switch (cipherHandle->algorithm)
    {
        case SEC_CIPHERALGORITHM_AES_CBC_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_ECB_NO_PADDING:
        case SEC_CIPHERALGORITHM_AES_ECB_PKCS7_PADDING:
        case SEC_CIPHERALGORITHM_AES_CBC_PKCS7_PADDING:
            if (EVP_CIPHER_CTX_cleanup(&(cipherHandle->evp_ctx)) != 1)
            {
                SEC_LOG_ERROR("EVP_CIPHER_CTX_cleanup failed");
            }
            break;

        case SEC_CIPHERALGORITHM_AES_CTR:
            Sec_Memset(&cipherHandle->ctr_ctx, 0, sizeof(cipherHandle->ctr_ctx));
            break;

        case SEC_CIPHERALGORITHM_RSA_PKCS1_PADDING:
        case SEC_CIPHERALGORITHM_RSA_OAEP_PADDING:
            break;

            /* NEW: other cipher algorithms */
        default:
            SEC_LOG_ERROR("Unimplemented cipher algorithm");
            goto unimplemented;
    }

    SEC_FREE(cipherHandle);
    return SEC_RESULT_SUCCESS;

    unimplemented: return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result SecDigest_GetInstance(Sec_ProcessorHandle* secProcHandle,
        Sec_DigestAlgorithm algorithm, Sec_DigestHandle** digestHandle)
{
    CHECK_HANDLE(secProcHandle);

    *digestHandle = calloc(1, sizeof(Sec_DigestHandle));
    if (NULL == *digestHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*digestHandle)->algorithm = algorithm;

    switch (algorithm)
    {
        case SEC_DIGESTALGORITHM_SHA1:
            if (1 != SHA1_Init(&((*digestHandle)->sha1_ctx)))
            {
                SEC_FREE(*digestHandle);
                return SEC_RESULT_FAILURE;
            }
            break;

        case SEC_DIGESTALGORITHM_SHA256:
            if (1 != SHA256_Init(&((*digestHandle)->sha256_ctx)))
            {
                SEC_FREE(*digestHandle);
                return SEC_RESULT_FAILURE;
            }
            break;

        default:
            SEC_LOG_ERROR("Unimplemented digest algorithm");
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;
    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecDigest_Update(Sec_DigestHandle* digestHandle, SEC_BYTE* input,
        SEC_SIZE inputSize)
{
    CHECK_HANDLE(digestHandle);

    switch (digestHandle->algorithm)
    {
        case SEC_DIGESTALGORITHM_SHA1:
            if (1 != SHA1_Update(&(digestHandle->sha1_ctx), input, inputSize))
            {
                return SEC_RESULT_FAILURE;
            }
            break;

        case SEC_DIGESTALGORITHM_SHA256:
            if (1
                    != SHA256_Update(&(digestHandle->sha256_ctx), input,
                            inputSize))
            {
                return SEC_RESULT_FAILURE;
            }
            break;

        default:
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;
    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecDigest_UpdateWithKey(Sec_DigestHandle* digestHandle,
        Sec_KeyHandle *key)
{
    Sec_Result res = SEC_RESULT_FAILURE;
    SEC_BYTE symetric_key[SEC_SYMETRIC_KEY_MAX_LEN];

    CHECK_HANDLE(digestHandle);

    if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(key, symetric_key, SecKey_GetKeyLen(key)))
    {
        SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
        goto done;
    }

    switch (digestHandle->algorithm)
    {
        case SEC_DIGESTALGORITHM_SHA1:
            if (1 != SHA1_Update(&(digestHandle->sha1_ctx),
                            symetric_key, SecKey_GetKeyLen(key)))
            {
                SEC_LOG_ERROR("SHA1_Update failed");
                goto done;
            }
            break;

        case SEC_DIGESTALGORITHM_SHA256:
            if (1 != SHA256_Update(&(digestHandle->sha256_ctx),
                    symetric_key, SecKey_GetKeyLen(key)))
            {
                SEC_LOG_ERROR("SHA256_Update failed");
                goto done;
            }
            break;

        default:
            SEC_LOG_ERROR("Unimplemented algorithm");
            goto done;
            break;
    }

    res = SEC_RESULT_SUCCESS;

done:
    Sec_Memset(symetric_key, 0, sizeof(symetric_key));
    return res;
}

Sec_Result SecDigest_Release(Sec_DigestHandle* digestHandle,
        SEC_BYTE* digestOutput, SEC_SIZE* digestSize)
{
    CHECK_HANDLE(digestHandle);

    switch (digestHandle->algorithm)
    {
        case SEC_DIGESTALGORITHM_SHA1:
            *digestSize = 20;
            if (1 != SHA1_Final(digestOutput, &(digestHandle->sha1_ctx)))
            {
                return SEC_RESULT_FAILURE;
            }
            break;

        case SEC_DIGESTALGORITHM_SHA256:
            *digestSize = 32;
            if (1 != SHA256_Final(digestOutput, &(digestHandle->sha256_ctx)))
            {
                return SEC_RESULT_FAILURE;
            }
            break;

        default:
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;
    }

    SEC_FREE(digestHandle);
    return SEC_RESULT_SUCCESS;
}

Sec_Result SecSignature_GetInstance(Sec_ProcessorHandle* secProcHandle,
        Sec_SignatureAlgorithm algorithm, Sec_SignatureMode mode,
        Sec_KeyHandle* key, Sec_SignatureHandle** signatureHandle)
{
    CHECK_HANDLE(secProcHandle);

    if (SEC_RESULT_SUCCESS
            != SecSignature_IsValidKey(key->key_data.info.key_type, algorithm, mode))
    {
        return SEC_RESULT_INVALID_PARAMETERS;
    }

    *signatureHandle = calloc(1, sizeof(Sec_SignatureHandle));
    if (NULL == *signatureHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*signatureHandle)->algorithm = algorithm;
    (*signatureHandle)->mode = mode;
    (*signatureHandle)->key_handle = key;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecSignature_Process(Sec_SignatureHandle* signatureHandle,
        SEC_BYTE* input, SEC_SIZE inputSize, SEC_BYTE* signature,
        SEC_SIZE *signatureSize)
{
    Sec_Result res;
    SEC_BYTE digest[SEC_DIGEST_MAX_LEN];
    SEC_SIZE digest_len;
    SEC_SIZE sig_size;
    Sec_RSARawPublicKey pubKey;
    int openssl_digest;
    int openssl_res;
    RSA *rsa = NULL;

    CHECK_HANDLE(signatureHandle);

    /* extract pub key */
    res = SecKey_ExtractPublicKey(signatureHandle->key_handle, &pubKey);
    if (res != SEC_RESULT_SUCCESS)
    {
        SEC_LOG_ERROR("SecKey_ExtractPublicKey failed");
        return res;
    }

    *signatureSize = SecKey_GetKeyLen(signatureHandle->key_handle);

    if (SecSignature_IsDigest(signatureHandle->algorithm))
    {
        if (inputSize != SecDigest_GetDigestLenForAlgorithm(SecSignature_GetDigestAlgorithm(signatureHandle->algorithm)))
        {
            SEC_LOG_ERROR("Invalid input length");
            return SEC_RESULT_FAILURE;
        }

        memcpy(digest, input, inputSize);
        digest_len = inputSize;
    }
    else
    {
        /* calculate digest */
        res = SecDigest_SingleInput(signatureHandle->key_handle->proc,
                SecSignature_GetDigestAlgorithm(signatureHandle->algorithm), input,
                inputSize, digest, &digest_len);
        if (res != SEC_RESULT_SUCCESS)
        {
            SEC_LOG_ERROR("SecDigest_SingleInput failed");
            return res;
        }
    }

    if (signatureHandle->mode == SEC_SIGNATUREMODE_SIGN)
    {
        if (signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA1_PKCS
                || signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA1_PKCS_DIGEST)
            openssl_digest = NID_sha1;
        else if (signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA256_PKCS
                || signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA256_PKCS_DIGEST)
            openssl_digest = NID_sha256;
        else
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;

        rsa = _Sec_RSAFromKeyHandle(signatureHandle->key_handle);
        if (NULL == rsa)
        {
            SEC_LOG_ERROR("_Sec_RSAFromKeyHandle failed");
            return SEC_RESULT_FAILURE;
        }

        openssl_res = RSA_sign(openssl_digest, digest, digest_len, signature, &sig_size, rsa);
        *signatureSize = sig_size;

        SEC_RSA_FREE(rsa);

        if (0 == openssl_res)
        {
            SEC_LOG_ERROR("RSA_sign failed");
            return SEC_RESULT_FAILURE;
        }
    }
    else
    {
        if (signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA1_PKCS
                || signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA1_PKCS_DIGEST)
            openssl_digest = NID_sha1;
        else if (signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA256_PKCS
                || signatureHandle->algorithm == SEC_SIGNATUREALGORITHM_RSA_SHA256_PKCS_DIGEST)
            openssl_digest = NID_sha256;
        else
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;

        rsa = SecUtils_RSAFromPubBinary(&pubKey);
        if (NULL == rsa)
        {
            SEC_LOG_ERROR("SecUtils_RSAFromPubBinary failed");
            return SEC_RESULT_FAILURE;
        }

        openssl_res = RSA_verify(openssl_digest, digest, digest_len, signature,
                *signatureSize, rsa);

        SEC_RSA_FREE(rsa);

        if (1 != openssl_res)
        {
            SEC_LOG_ERROR("RSA_verify failed");
            SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
            return SEC_RESULT_VERIFICATION_FAILED;
        }

    }

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecSignature_Release(Sec_SignatureHandle* signatureHandle)
{
    CHECK_HANDLE(signatureHandle);
    SEC_FREE(signatureHandle);
    return SEC_RESULT_SUCCESS;
}

Sec_Result SecMac_GetInstance(Sec_ProcessorHandle* secProcHandle,
        Sec_MacAlgorithm algorithm, Sec_KeyHandle* key,
        Sec_MacHandle** macHandle)
{
    SEC_BYTE symetric_key[SEC_SYMETRIC_KEY_MAX_LEN];
    Sec_Result res = SEC_RESULT_FAILURE;

    CHECK_HANDLE(secProcHandle);

    *macHandle = NULL;

    if (SEC_RESULT_SUCCESS
            != SecMac_IsValidKey(key->key_data.info.key_type, algorithm))
    {
        SEC_LOG_ERROR("Not a valid mac key");
        goto done;
    }

    *macHandle = calloc(1, sizeof(Sec_MacHandle));
    if (NULL == *macHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        goto done;
    }

    (*macHandle)->algorithm = algorithm;
    (*macHandle)->key_handle = key;

    if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(key, symetric_key, SecKey_GetKeyLen(key)))
    {
        SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
        goto done;
    }

    switch (algorithm)
    {
        case SEC_MACALGORITHM_HMAC_SHA1:
        case SEC_MACALGORITHM_HMAC_SHA256:
            HMAC_CTX_init(&((*macHandle)->hmac_ctx));
            HMAC_Init(&((*macHandle)->hmac_ctx), symetric_key,
                    SecKey_GetKeyLen(key), (algorithm == SEC_MACALGORITHM_HMAC_SHA1) ? EVP_sha1() : EVP_sha256());
            break;

        case SEC_MACALGORITHM_CMAC_AES_128:
            CMAC_CTX_init(&((*macHandle)->cmac_ctx));
            if (1 != CMAC_Init(&((*macHandle)->cmac_ctx), symetric_key,
                            SecKey_GetKeyLen(key), EVP_aes_128_ecb(), NULL ))
            {
                SEC_LOG_ERROR("CMAC_Init failed");
                goto done;
            }
            break;

        default:
            SEC_LOG_ERROR("Unimplemented mac algorithm");
            goto done;
    }

    res = SEC_RESULT_SUCCESS;

done:
    if (res != SEC_RESULT_SUCCESS)
    {
        SEC_FREE(*macHandle);
    }
    Sec_Memset(symetric_key, 0, sizeof(symetric_key));
    return res;
}

Sec_Result SecMac_Update(Sec_MacHandle* macHandle, SEC_BYTE* input,
        SEC_SIZE inputSize)
{
    CHECK_HANDLE(macHandle);

    switch (macHandle->algorithm)
    {
        case SEC_MACALGORITHM_HMAC_SHA1:
        case SEC_MACALGORITHM_HMAC_SHA256:
            HMAC_Update(&macHandle->hmac_ctx, input, inputSize);
            break;

        case SEC_MACALGORITHM_CMAC_AES_128:
            CMAC_Update(&macHandle->cmac_ctx, input, inputSize);
            break;

        default:
            SEC_LOG_ERROR("Unimplemented mac algorithm");
            goto unimplemented;
    }

    return SEC_RESULT_SUCCESS;
    unimplemented: return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result SecMac_UpdateWithKey(Sec_MacHandle* macHandle,
        Sec_KeyHandle *keyHandle)
{
    Sec_Result res = SEC_RESULT_FAILURE;
    SEC_BYTE symetric_key[SEC_SYMETRIC_KEY_MAX_LEN];

    CHECK_HANDLE(macHandle);

    if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(macHandle->key_handle,
            symetric_key, SecKey_GetKeyLen(macHandle->key_handle)))
    {
        SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
        goto done;
    }

    switch (macHandle->algorithm)
    {
        case SEC_MACALGORITHM_HMAC_SHA1:
        case SEC_MACALGORITHM_HMAC_SHA256:
            HMAC_Update(&macHandle->hmac_ctx, symetric_key,
                    SecKey_GetKeyLen(keyHandle));
            break;

        case SEC_MACALGORITHM_CMAC_AES_128:
            CMAC_Update(&macHandle->cmac_ctx, symetric_key,
                    SecKey_GetKeyLen(keyHandle));
            break;

        default:
            SEC_LOG_ERROR("Unimplemented mac algorithm");
            goto done;
    }

    res = SEC_RESULT_SUCCESS;

done:
    Sec_Memset(symetric_key, 0, sizeof(symetric_key));
    return res;
}

Sec_Result SecMac_Release(Sec_MacHandle* macHandle, SEC_BYTE* macBuffer,
        SEC_SIZE* macSize)
{
    SEC_SIZE out_len;

    CHECK_HANDLE(macHandle);

    switch (macHandle->algorithm)
    {
        case SEC_MACALGORITHM_HMAC_SHA1:
        case SEC_MACALGORITHM_HMAC_SHA256:
            HMAC_Final(&(macHandle->hmac_ctx), macBuffer, &out_len);
            *macSize = out_len;
            HMAC_CTX_cleanup(&(macHandle->hmac_ctx));
            break;

        case SEC_MACALGORITHM_CMAC_AES_128:
            CMAC_Final(&macHandle->cmac_ctx, macBuffer, &out_len);
            *macSize = out_len;
            break;

        default:
            SEC_LOG_ERROR("Unimplemented mac algorithm");
            return SEC_RESULT_UNIMPLEMENTED_FEATURE;
    }

    SEC_FREE(macHandle);
    return SEC_RESULT_SUCCESS;
}

Sec_Result SecRandom_GetInstance(Sec_ProcessorHandle* secProcHandle,
        Sec_RandomAlgorithm algorithm, Sec_RandomHandle** randomHandle)
{
    CHECK_HANDLE(secProcHandle);

    *randomHandle = calloc(1, sizeof(Sec_RandomHandle));
    if (NULL == *randomHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*randomHandle)->algorithm = algorithm;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecRandom_Process(Sec_RandomHandle* randomHandle, SEC_BYTE* output,
        SEC_SIZE outputSize)
{
    CHECK_HANDLE(randomHandle);

    switch (randomHandle->algorithm)
    {
        case SEC_RANDOMALGORITHM_TRUE:
            CHECK_EXACT(RAND_bytes(output, outputSize), 1, error);
            break;

        case SEC_RANDOMALGORITHM_PRNG:
            CHECK_EXACT(RAND_pseudo_bytes(output, outputSize), 1, error);
            break;

        default:
            SEC_LOG_ERROR("Unimplemented random algorithm");
            goto unimplemented;
    }

    return SEC_RESULT_SUCCESS;
    error: return SEC_RESULT_FAILURE;
    unimplemented: return SEC_RESULT_UNIMPLEMENTED_FEATURE;
}

Sec_Result SecRandom_Release(Sec_RandomHandle* randomHandle)
{
    CHECK_HANDLE(randomHandle);
    SEC_FREE(randomHandle);
    return SEC_RESULT_SUCCESS;
}

SEC_SIZE SecCertificate_List(Sec_ProcessorHandle *proc, SEC_OBJECTID *items, SEC_SIZE maxNumItems)
{
    _Sec_RAMCertificateData *cert;
    SEC_SIZE numItems = 0;

    /* look in RAM */
    cert = proc->ram_certs;
    while (cert != NULL)
    {
        numItems = SecUtils_UpdateItemList(items, maxNumItems, numItems, cert->object_id);
        cert = cert->next;
    }

    /* look in file system */
    numItems = SecUtils_UpdateItemListFromDir(items, maxNumItems, numItems, proc->certstorage_file_dir, SEC_CERT_FILENAME_EXT);

    /* look in OEM memory */
#ifdef SEC_ENABLE_OEM_PROVISIONING
#endif

    return numItems;
}

Sec_Result SecCertificate_GetInstance(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_CertificateHandle** certHandle)
{
    Sec_Result result;
    _Sec_CertificateData cert_data;
    Sec_StorageLoc location;

    CHECK_HANDLE(secProcHandle);

    if (object_id == SEC_OBJECTID_INVALID)
    {
        SEC_LOG_ERROR("Invalid object_id");
        return SEC_RESULT_INVALID_PARAMETERS;
    }

    result = _Sec_RetrieveCertificateData(secProcHandle, object_id, &location,
            &cert_data);
    if (result != SEC_RESULT_SUCCESS)
    {
        return result;
    }

    result = _Sec_ValidateCertificateData(secProcHandle, &cert_data);
    if (result != SEC_RESULT_SUCCESS)
    {
        SEC_LOG_ERROR("_Sec_ValidateCertificateData failed");
        return SEC_RESULT_VERIFICATION_FAILED;
    }

    *certHandle = calloc(1, sizeof(Sec_CertificateHandle));
    if (NULL == *certHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*certHandle)->object_id = object_id;
    memcpy(&((*certHandle)->cert_data), &cert_data,
            sizeof(_Sec_CertificateData));
    (*certHandle)->location = location;
    (*certHandle)->proc = secProcHandle;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecCertificate_Provision(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location,
        Sec_CertificateContainer data_type, SEC_BYTE *data, SEC_SIZE data_len)
{
    _Sec_CertificateData cert_data;
    Sec_Result result;

    CHECK_HANDLE(secProcHandle);

    if (object_id == SEC_OBJECTID_INVALID)
    {
        SEC_LOG_ERROR("Cannot provision object with SEC_OBJECTID_INVALID");
        return SEC_RESULT_FAILURE;
    }

    result = _Sec_ProcessCertificateContainer(secProcHandle, &cert_data,
            data_type, data, data_len);
    if (SEC_RESULT_SUCCESS != result)
        return result;

    return _Sec_StoreCertificateData(secProcHandle, object_id, location,
            &cert_data);
}

Sec_Result SecCertificate_Delete(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id)
{
    char file_name[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMCertificateData *ram_cert = NULL;
    _Sec_RAMCertificateData *ram_cert_parent = NULL;
    SEC_SIZE certs_found = 0;
    SEC_SIZE certs_deleted = 0;

    CHECK_HANDLE(secProcHandle);

    /* ram */
    _Sec_FindRAMCertificateData(secProcHandle, object_id, &ram_cert,
            &ram_cert_parent);
    if (ram_cert != NULL )
    {
        if (ram_cert_parent == NULL )
            secProcHandle->ram_certs = ram_cert->next;
        else
            ram_cert_parent->next = ram_cert->next;

        Sec_Memset(ram_cert, 0, sizeof(_Sec_RAMCertificateData));

        SEC_FREE(ram_cert);

        ++certs_found;
        ++certs_deleted;
    }

    /* file system */
    snprintf(file_name, sizeof(file_name), "%s" SEC_CERT_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
            object_id);
    if (SecUtils_FileExists(file_name))
    {
        SecUtils_RmFile(file_name);
        ++certs_found;

        if (!SecUtils_FileExists(file_name))
            ++certs_deleted;
    }

    snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_CERTINFO_FILENAME_PATTERN, secProcHandle->certstorage_file_dir,
            object_id);
    if (!SecUtils_FileExists(file_name) && SecUtils_FileExists(file_name_info))
    {
        SecUtils_RmFile(file_name_info);
    }

    /* soc */

    if (certs_found == 0)
        return SEC_RESULT_NO_SUCH_ITEM;

    if (certs_found != certs_deleted)
        return SEC_RESULT_ITEM_NON_REMOVABLE;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecCertificate_ExtractPublicKey(Sec_CertificateHandle* cert_handle,
        Sec_RSARawPublicKey *public_key)
{
    _Sec_CertificateData *cert_data;
    X509 *x509 = NULL;
    EVP_PKEY *evp_key = NULL;
    RSA *rsa = NULL;

    CHECK_HANDLE(cert_handle);

    cert_data = &(cert_handle->cert_data);

    x509 = SecCertificate_DerToX509(cert_data->cert, cert_data->cert_len);

    if (NULL == x509)
    {
        SEC_LOG_ERROR(
                "Could not load X509 certificate from _Sec_CertificateData");
        goto error;
    }

    evp_key = X509_get_pubkey(x509);
    if (evp_key == NULL )
    {
        SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
        goto error;
    }

    rsa = EVP_PKEY_get1_RSA(evp_key);
    if (rsa == NULL )
    {
        SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
        goto error;
    }

    Sec_Uint32ToBEBytes(RSA_size(rsa), public_key->modulus_len_be);
    SecUtils_BigNumToBuffer(rsa->n, public_key->n, Sec_BEBytesToUint32(public_key->modulus_len_be));
    SecUtils_BigNumToBuffer(rsa->e, public_key->e, 4);

    SEC_EVPPKEY_FREE(evp_key);
    evp_key = NULL;
    SEC_RSA_FREE(rsa);
    rsa = NULL;
    SEC_X509_FREE(x509);
    x509 = NULL;

    return SEC_RESULT_SUCCESS;

    error: if (x509 != NULL )
        SEC_X509_FREE(x509);
    if (evp_key != NULL )
        SEC_EVPPKEY_FREE(evp_key);
    if (rsa != NULL )
        SEC_RSA_FREE(rsa);
    return SEC_RESULT_FAILURE;
}

Sec_Result _Sec_VerifyCertificateWithRSA(Sec_CertificateHandle* cert_handle,
        RSA* rsa)
{
    EVP_PKEY *evp_key = NULL;
    X509 *x509 = NULL;
    int verify_res;

    x509 = SecCertificate_DerToX509(&cert_handle->cert_data.cert,
            cert_handle->cert_data.cert_len);

    if (x509 == NULL )
    {
        SEC_LOG_ERROR("SecCertificate_DerToX509 failed");
        goto error;
    }

    if (rsa == NULL )
    {
        SEC_LOG_ERROR("_Sec_ReadRSAPublic failed");
        goto error;
    }

    evp_key = EVP_PKEY_new();
    if (0 == EVP_PKEY_set1_RSA(evp_key, rsa))
    {
        SEC_LOG_ERROR("EVP_PKEY_set1_RSA failed");
        goto error;
    }

    verify_res = X509_verify(x509, evp_key);

    SEC_X509_FREE(x509);
    SEC_EVPPKEY_FREE(evp_key);

    if (1 != verify_res)
    {
        SEC_LOG_ERROR("X509_verify failed");
        SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
        return SEC_RESULT_VERIFICATION_FAILED;
    }

    return SEC_RESULT_SUCCESS;

    error: if (x509 != NULL )
        SEC_X509_FREE(x509);
    if (evp_key != NULL )
        SEC_EVPPKEY_FREE(evp_key);

    return SEC_RESULT_FAILURE;
}

Sec_Result SecCertificate_Verify(Sec_CertificateHandle* cert_handle,
        Sec_KeyHandle* key_handle)
{
    Sec_RSARawPublicKey rsaPubKey;

    CHECK_HANDLE(cert_handle);
    CHECK_HANDLE(key_handle);

    if (SEC_RESULT_SUCCESS != SecKey_ExtractPublicKey(key_handle, &rsaPubKey))
    {
        SEC_LOG_ERROR("SecKey_ExtractPublicKey failed");
        return SEC_RESULT_FAILURE;

    }

    return SecCertificate_VerifyWithRawPublicKey(cert_handle, &rsaPubKey);
}

Sec_Result SecCertificate_VerifyWithRawPublicKey(
        Sec_CertificateHandle* cert_handle, Sec_RSARawPublicKey* public_key)
{
    X509 *x509 = NULL;
    Sec_Result res;

    CHECK_HANDLE(cert_handle);

    x509 = SecCertificate_DerToX509(&cert_handle->cert_data.cert,
            cert_handle->cert_data.cert_len);

    if (x509 == NULL)
    {
        SEC_LOG_ERROR("SecCertificate_DerToX509 failed");
        return SEC_RESULT_FAILURE;
    }

    res = SecUtils_VerifyX509WithRawPublicKey(x509, public_key);
    SEC_X509_FREE(x509);

    return res;
}

Sec_Result SecCertificate_Export(Sec_CertificateHandle* cert_handle,
        SEC_BYTE *buffer, SEC_SIZE buffer_len, SEC_SIZE *written)
{
    CHECK_HANDLE(cert_handle);

    if (buffer == NULL)
    {
        *written = cert_handle->cert_data.cert_len;
        return SEC_RESULT_SUCCESS;
    }

    if (buffer_len < cert_handle->cert_data.cert_len)
        return SEC_RESULT_BUFFER_TOO_SMALL;

    memcpy(buffer, cert_handle->cert_data.cert,
            cert_handle->cert_data.cert_len);
    *written = cert_handle->cert_data.cert_len;
    return SEC_RESULT_SUCCESS;
}

Sec_Result SecCertificate_Release(Sec_CertificateHandle* certHandle)
{
    CHECK_HANDLE(certHandle);
    SEC_FREE(certHandle);
    return SEC_RESULT_SUCCESS;
}

SEC_SIZE SecKey_GetKeyLen(Sec_KeyHandle *keyHandle)
{
    return SecKey_GetKeyLenForKeyType(keyHandle->key_data.info.key_type);
}

Sec_Result SecKey_GetInstance(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_KeyHandle **keyHandle)
{
    Sec_Result result;
    _Sec_KeyData key_data;
    Sec_StorageLoc location;

    CHECK_HANDLE(secProcHandle);

    if (object_id == SEC_OBJECTID_INVALID)
        return SEC_RESULT_INVALID_PARAMETERS;

    result = _Sec_RetrieveKeyData(secProcHandle, object_id, &location,
            &key_data);
    if (result != SEC_RESULT_SUCCESS)
        return result;

    *keyHandle = calloc(1, sizeof(Sec_KeyHandle));
    if (NULL == *keyHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*keyHandle)->object_id = object_id;
    memcpy(&((*keyHandle)->key_data), &key_data, sizeof(_Sec_KeyData));
    (*keyHandle)->location = location;
    (*keyHandle)->proc = secProcHandle;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecKey_ExtractPublicKey(Sec_KeyHandle* key_handle,
        Sec_RSARawPublicKey *public_key)
{
    RSA *rsa = NULL;

    CHECK_HANDLE(key_handle);

    rsa = _Sec_RSAFromKeyHandle(key_handle);
    if (NULL == rsa)
    {
        SEC_LOG_ERROR("_Sec_RSAFromKeyHandle failed");
        return SEC_RESULT_FAILURE;
    }

    SecUtils_RSAToPubBinary(rsa, public_key);
    SEC_RSA_FREE(rsa);

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecKey_Generate(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_KeyType keyType, Sec_StorageLoc location)
{
    Sec_KeyHandle *keyHandle;
    RSA *rsa = NULL;
    SEC_BYTE symetric_key[SEC_SYMETRIC_KEY_MAX_LEN];
    Sec_RSARawPrivateKey priv;
    Sec_Result res = SEC_RESULT_FAILURE;

    CHECK_HANDLE(secProcHandle);

    if (SEC_RESULT_SUCCESS
            == SecKey_GetInstance(secProcHandle, object_id, &keyHandle)
            && keyHandle->location != SEC_STORAGELOC_OEM)
    {
        SEC_LOG_ERROR("Item has already been provisioned");
        SecKey_Release(keyHandle);
        return SEC_RESULT_ITEM_ALREADY_PROVISIONED;
    }

    switch (keyType)
    {
        case SEC_KEYTYPE_AES_128:
        case SEC_KEYTYPE_AES_256:
        case SEC_KEYTYPE_HMAC_128:
        case SEC_KEYTYPE_HMAC_160:
        case SEC_KEYTYPE_HMAC_256:
            if (1 != RAND_bytes(symetric_key, SecKey_GetKeyLenForKeyType(keyType)))
            {
                SEC_LOG_ERROR("RAND_bytes failed");
                goto done;
            }
            if (SEC_RESULT_SUCCESS != SecKey_Provision(secProcHandle, object_id, location, SecKey_GetClearContainer(keyType), symetric_key, SecKey_GetKeyLenForKeyType(keyType)))
            {
                SEC_LOG_ERROR("SecKey_Provision failed");
                goto done;
            }
            break;

        case SEC_KEYTYPE_RSA_1024:
        case SEC_KEYTYPE_RSA_2048:
            rsa = RSA_generate_key(SecKey_GetKeyLenForKeyType(keyType) * 8,
                    65537, NULL, NULL );
            if (rsa == NULL )
            {
                SEC_LOG_ERROR("RSA_generate_key failed");
                SEC_LOG_ERROR("%s", ERR_error_string(ERR_get_error(), NULL));
                goto done;
            }

            /* write private */
            SecUtils_RSAToPrivBinary(rsa, &priv);
            SEC_RSA_FREE(rsa);

            if (SEC_RESULT_SUCCESS != SecKey_Provision(secProcHandle, object_id, location, SecKey_GetClearContainer(keyType), (SEC_BYTE*) &priv, sizeof(priv)))
            {
                SEC_LOG_ERROR("SecKey_Provision failed");
                goto done;
            }
            break;

        default:
            SEC_LOG_ERROR("Unimplemented feature");
            goto done;
            break;
    }

    res = SEC_RESULT_SUCCESS;
done:
    Sec_Memset(symetric_key, 0, sizeof(symetric_key));
    Sec_Memset(&priv, 0, sizeof(priv));

    return res;
}

Sec_Result SecKey_Provision(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location, Sec_KeyContainer data_type,
        SEC_BYTE *data, SEC_SIZE data_len)
{
    _Sec_KeyData key_data;
    Sec_Result result;

    CHECK_HANDLE(secProcHandle);

    result = SecOpenSSL_ProcessKeyContainer(secProcHandle, &key_data, data_type, data,
            data_len, object_id);
    if (SEC_RESULT_SUCCESS != result)
        return result;

    return _Sec_StoreKeyData(secProcHandle, object_id, location, &key_data);
}

Sec_Result SecKey_Delete(Sec_ProcessorHandle* secProcHandle, SEC_OBJECTID object_id)
{
    char file_name[SEC_MAX_FILE_PATH_LEN];
    char file_name_info[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMKeyData *ram_key = NULL;
    _Sec_RAMKeyData *ram_key_parent = NULL;
    SEC_SIZE keys_found = 0;
    SEC_SIZE keys_deleted = 0;

    CHECK_HANDLE(secProcHandle);

    /* ram */
    _Sec_FindRAMKeyData(secProcHandle, object_id, &ram_key, &ram_key_parent);
    if (ram_key != NULL )
    {
        if (ram_key_parent == NULL )
            secProcHandle->ram_keys = ram_key->next;
        else
            ram_key_parent->next = ram_key->next;

        Sec_Memset(ram_key, 0, sizeof(_Sec_RAMKeyData));

        SEC_FREE(ram_key);

        ++keys_found;
        ++keys_deleted;
    }

    /* file system */
    snprintf(file_name, sizeof(file_name), "%s" SEC_KEY_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
            object_id);
    if (SecUtils_FileExists(file_name))
    {
        SecUtils_RmFile(file_name);
        ++keys_found;

        if (!SecUtils_FileExists(file_name))
            ++keys_deleted;
    }

    snprintf(file_name_info, sizeof(file_name_info), "%s" SEC_KEYINFO_FILENAME_PATTERN, secProcHandle->keystorage_file_dir,
            object_id);
    if (!SecUtils_FileExists(file_name) && SecUtils_FileExists(file_name_info))
    {
        SecUtils_RmFile(file_name_info);
    }

    /* soc */
    if (keys_found == 0)
        return SEC_RESULT_NO_SUCH_ITEM;

    if (keys_found != keys_deleted)
        return SEC_RESULT_ITEM_NON_REMOVABLE;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecKey_Release(Sec_KeyHandle* keyHandle)
{
    CHECK_HANDLE(keyHandle);

    SEC_FREE(keyHandle);

    return SEC_RESULT_SUCCESS;
}

Sec_KeyType _Sec_GetOutputMacKeyType(Sec_MacAlgorithm alg)
{
    switch (alg)
    {
        case SEC_MACALGORITHM_HMAC_SHA1:
            return SEC_KEYTYPE_HMAC_160;
        case SEC_MACALGORITHM_HMAC_SHA256:
            return SEC_KEYTYPE_HMAC_256;
        case SEC_MACALGORITHM_CMAC_AES_128:
            return SEC_KEYTYPE_AES_128;
        default:
            break;
    }

    return SEC_KEYTYPE_NUM;
}

Sec_Result SecKey_Derive_HKDF(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id_derived, Sec_KeyType type_derived,
        Sec_StorageLoc loc_derived, Sec_MacAlgorithm macAlgorithm,
        SEC_BYTE *nonce,
        SEC_BYTE *salt, SEC_SIZE saltSize, SEC_BYTE *info, SEC_SIZE infoSize)
{
    int r, i;
    SEC_SIZE key_length;
    SEC_BYTE out_key[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_SIZE digest_length;
    SEC_BYTE prk[SEC_MAC_MAX_LEN];
    SEC_SIZE prk_len;
    SEC_BYTE t[SEC_MAC_MAX_LEN];
    SEC_SIZE t_len = 0;
    SEC_SIZE cp_len;
    Sec_KeyHandle *prk_key = NULL;
    Sec_MacHandle *mac_handle = NULL;
    SEC_BYTE loop;
    SEC_OBJECTID temp_key_id = SEC_OBJECTID_INVALID;

    if (!SecKey_IsSymetric(type_derived))
    {
        SEC_LOG_ERROR("Only symetric keys can be derived");
        return SEC_RESULT_INVALID_PARAMETERS;
    }

    /* provision base key */
    CHECK_EXACT(_Sec_ProvisionBaseKey(secProcHandle, nonce), SEC_RESULT_SUCCESS, error);

    key_length = SecKey_GetKeyLenForKeyType(type_derived);
    digest_length = SecDigest_GetDigestLenForAlgorithm(
            SecMac_GetDigestAlgorithm(macAlgorithm));

    /* Extract */
    CHECK_EXACT(
            SecMac_SingleInputId(secProcHandle, macAlgorithm, SEC_OBJECTID_BASE_KEY_MAC, salt, saltSize, prk, &prk_len),
            SEC_RESULT_SUCCESS, error);

    temp_key_id = SEC_OBJECTID_OPENSSL_DERIVE_TMP;
    CHECK_EXACT(
            SecKey_Provision(secProcHandle, temp_key_id, SEC_STORAGELOC_RAM_SOFT_WRAPPED, SecKey_GetClearContainer( _Sec_GetOutputMacKeyType(macAlgorithm)), prk, prk_len),
            SEC_RESULT_SUCCESS, error);

    CHECK_EXACT(
            SecKey_GetInstance(secProcHandle, temp_key_id, &prk_key),
            SEC_RESULT_SUCCESS, error);

    /* Expand */
    r = key_length / digest_length
            + ((key_length % digest_length == 0) ? 0 : 1);

    for (i = 1; i <= r; i++)
    {
        loop = i;

        if (i == r)
            cp_len = key_length % digest_length;
        else
            cp_len = digest_length;

        if (SEC_RESULT_SUCCESS
                != SecMac_GetInstance(secProcHandle, macAlgorithm, prk_key,
                        &mac_handle))
            goto error;

        if (t_len > 0
                && SEC_RESULT_SUCCESS != SecMac_Update(mac_handle, t, t_len))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Update(mac_handle, info, infoSize))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Update(mac_handle, &loop, 1))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Release(mac_handle, t, &t_len))
        {
            mac_handle = NULL;
            goto error;
        }

        memcpy(out_key + (i - 1) * digest_length, t, cp_len);
    }

    SecKey_Release(prk_key);
    prk_key = NULL;
    SecKey_Delete(secProcHandle, temp_key_id);

    /* store key */
    CHECK_EXACT(
            SecKey_Provision(secProcHandle, object_id_derived, loc_derived, SecKey_GetClearContainer(type_derived), out_key, key_length),
            SEC_RESULT_SUCCESS, error);

    Sec_Memset(out_key, 0, sizeof(out_key));
    Sec_Memset(prk, 0, sizeof(prk));
    Sec_Memset(t, 0, sizeof(t));

    return SEC_RESULT_SUCCESS;

error:
    if (mac_handle != NULL )
        SecMac_Release(mac_handle, t, &t_len);
    if (prk_key != NULL )
        SecKey_Release(prk_key);

    SecKey_Delete(secProcHandle, temp_key_id);

    Sec_Memset(out_key, 0, sizeof(out_key));
    Sec_Memset(prk, 0, sizeof(prk));
    Sec_Memset(t, 0, sizeof(t));

    return SEC_RESULT_FAILURE;
}

Sec_Result SecKey_Derive_ConcatKDF(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id_derived, Sec_KeyType type_derived,
        Sec_StorageLoc loc_derived, Sec_DigestAlgorithm digestAlgorithm,
        SEC_BYTE *nonce,
        SEC_BYTE *otherInfo, SEC_SIZE otherInfoSize)
{
    int i;
    SEC_BYTE loop[] =
        { 0, 0, 0, 0 };
    SEC_BYTE hash[SEC_DIGEST_MAX_LEN];
    SEC_SIZE key_length;
    SEC_SIZE digest_length;
    int r;
    Sec_KeyHandle *base_key = NULL;
    Sec_DigestHandle *digestHandle = NULL;
    SEC_BYTE out_key[SEC_SYMETRIC_KEY_MAX_LEN];
    Sec_Result res = SEC_RESULT_FAILURE;

    if (!SecKey_IsSymetric(type_derived))
    {
        SEC_LOG_ERROR("Can only derive symetric keys");
        return SEC_RESULT_INVALID_PARAMETERS;
    }

    /* provision base key */
    CHECK_EXACT(_Sec_ProvisionBaseKey(secProcHandle, nonce), SEC_RESULT_SUCCESS,
            done);

    key_length = SecKey_GetKeyLenForKeyType(type_derived);
    digest_length = SecDigest_GetDigestLenForAlgorithm(digestAlgorithm);
    r = key_length / digest_length
            + ((key_length % digest_length == 0) ? 0 : 1);

    CHECK_EXACT(
            SecKey_GetInstance(secProcHandle, SEC_OBJECTID_BASE_KEY_AES, &base_key),
            SEC_RESULT_SUCCESS, done);

    for (i = 1; i <= r; ++i)
    {
        loop[3] = i;

        CHECK_EXACT(
                SecDigest_GetInstance(secProcHandle, digestAlgorithm, &digestHandle),
                SEC_RESULT_SUCCESS, done);
        CHECK_EXACT( SecDigest_Update(digestHandle, loop, sizeof(loop)),
                SEC_RESULT_SUCCESS, done);
        CHECK_EXACT( SecDigest_UpdateWithKey(digestHandle, base_key),
                SEC_RESULT_SUCCESS, done);
        CHECK_EXACT(
                SecDigest_Update(digestHandle, otherInfo, otherInfoSize),
                SEC_RESULT_SUCCESS, done);

        if (SEC_RESULT_SUCCESS != SecDigest_Release(digestHandle, hash, &digest_length))
        {
            SEC_LOG_ERROR("SecDigest_Release failed");
            digestHandle = NULL;
            goto done;
        }
        digestHandle = NULL;

        if (i < r || (key_length % digest_length == 0))
        {
            memcpy(out_key + digest_length * (i - 1), hash,
                    digest_length);
        }
        else
        {
            memcpy(out_key + digest_length * (i - 1), hash,
                    key_length % digest_length);
        }
    }

    /* store key */
    CHECK_EXACT(
            SecKey_Provision(secProcHandle, object_id_derived, loc_derived, SecKey_GetClearContainer(type_derived), out_key, key_length),
            SEC_RESULT_SUCCESS, done);

    res = SEC_RESULT_SUCCESS;

done:
    Sec_Memset(out_key, 0, sizeof(out_key));
    if (base_key != NULL )
        SecKey_Release(base_key);

    if (digestHandle != NULL )
        SecDigest_Release(digestHandle, hash, &digest_length);

    return res;
}

Sec_Result SecKey_Derive_PBEKDF(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id_derived, Sec_KeyType type_derived,
        Sec_StorageLoc loc_derived, Sec_MacAlgorithm macAlgorithm,
        SEC_BYTE *nonce,
        SEC_BYTE *salt, SEC_SIZE saltSize, SEC_SIZE numIterations)
{

    SEC_SIZE key_length;
    SEC_SIZE digest_length;
    SEC_SIZE i, j, k, l;
    SEC_BYTE loop[] =
        { 0, 0, 0, 0 };
    SEC_BYTE mac1[SEC_MAC_MAX_LEN];
    SEC_BYTE mac2[SEC_MAC_MAX_LEN];
    SEC_BYTE out[SEC_MAC_MAX_LEN];
    SEC_SIZE mac1_len;
    SEC_SIZE mac2_len;
    SEC_SIZE cp_len;
    SEC_BYTE out_key[SEC_AES_KEY_MAX_LEN];
    Sec_MacHandle *mac_handle = NULL;
    Sec_KeyHandle *base_key = NULL;

    if (!SecKey_IsSymetric(type_derived))
    {
        SEC_LOG_ERROR("Only symetric keys can be derived");
        return SEC_RESULT_INVALID_PARAMETERS;
    }

    /* provision base key */
    CHECK_EXACT(_Sec_ProvisionBaseKey(secProcHandle, nonce), SEC_RESULT_SUCCESS,
            error);

    key_length = SecKey_GetKeyLenForKeyType(type_derived);
    digest_length = SecDigest_GetDigestLenForAlgorithm(
            SecMac_GetDigestAlgorithm(macAlgorithm));

    l = key_length / digest_length
            + ((key_length % digest_length == 0) ? 0 : 1);

    CHECK_EXACT(
            SecKey_GetInstance(secProcHandle, SEC_OBJECTID_BASE_KEY_MAC, &base_key),
            SEC_RESULT_SUCCESS, error);

    for (i = 1; i <= l; i++)
    {
        loop[3] = i;

        if (i == l)
            cp_len = key_length % digest_length;
        else
            cp_len = digest_length;

        if (SEC_RESULT_SUCCESS
                != SecMac_GetInstance(secProcHandle, macAlgorithm, base_key,
                        &mac_handle))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Update(mac_handle, salt, saltSize))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Update(mac_handle, loop, sizeof(loop)))
            goto error;

        if (SEC_RESULT_SUCCESS != SecMac_Release(mac_handle, mac1, &mac1_len))
        {
            mac_handle = NULL;
            goto error;
        }

        memcpy(out, mac1, digest_length);

        for (j = 1; j < numIterations; j++)
        {
            if (SEC_RESULT_SUCCESS
                    != SecMac_SingleInput(secProcHandle, macAlgorithm, base_key,
                            mac1, digest_length, mac2, &mac2_len))
            {
                SEC_LOG_ERROR("SecMac_SingleInput failed");
                goto error;
            }

            memcpy(mac1, mac2, digest_length);

            for (k = 0; k < digest_length; ++k)
            {
                out[k] ^= mac1[k];
            }
        }

        memcpy(out_key + (i - 1) * digest_length, out, cp_len);

        Sec_Memset(mac1, 0, sizeof(mac1));
        Sec_Memset(mac2, 0, sizeof(mac2));
        Sec_Memset(out, 0, sizeof(out));
    }

    SecKey_Release(base_key);
    base_key = NULL;

    /* store key */
    CHECK_EXACT(
            SecKey_Provision(secProcHandle, object_id_derived, loc_derived, SecKey_GetClearContainer(type_derived), out_key, key_length),
            SEC_RESULT_SUCCESS, error);

    Sec_Memset(out_key, 0, sizeof(out_key));

    return SEC_RESULT_SUCCESS;

    error: if (mac_handle != NULL)
        SecMac_Release(mac_handle, mac1, &mac1_len);
    if (base_key != NULL)
        SecKey_Release(base_key);

    return SEC_RESULT_FAILURE;
}

Sec_Result SecKey_Derive_VendorAes128(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id_derived, Sec_StorageLoc loc_derived, SEC_BYTE *input, SEC_SIZE input_len)
{
    SecOpenSSL_DerivedInputs derived;
    SEC_BYTE digest[SEC_DIGEST_MAX_LEN];
    SEC_SIZE digest_len;

    if (SEC_RESULT_SUCCESS != SecDigest_SingleInput(secProcHandle, SEC_DIGESTALGORITHM_SHA256,
            input, input_len, digest, &digest_len))
    {
        SEC_LOG_ERROR("SecDigest_SingleInput failed");
        return SEC_RESULT_FAILURE;
    }

    /* setup key ladder inputs */
    memcpy(derived.input1, digest, 16);
    memcpy(derived.input2, digest+16, 16);

    return SecKey_Provision(secProcHandle, object_id_derived, loc_derived, SEC_OPENSSL_KEYCONTAINER_DERIVED, (SEC_BYTE *) &derived, sizeof(derived));
}

Sec_Result SecKey_Derive_KeyLadderAes128(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id_derived, Sec_StorageLoc loc_derived,
        Sec_KeyLadderRoot root, SEC_BYTE *input1, SEC_BYTE *input2, SEC_BYTE *input3, SEC_BYTE *input4)
{
    SecOpenSSL_DerivedInputs derived;

    if (root == SEC_KEYLADDERROOT_UNIQUE)
    {
        if (input1 == NULL)
        {
            SEC_LOG_ERROR("input1 is NULL");
            return SEC_RESULT_FAILURE;
        }

        if (input2 == NULL)
        {
            SEC_LOG_ERROR("input2 is NULL");
            return SEC_RESULT_FAILURE;
        }

        if (input3 != NULL)
        {
            SEC_LOG_ERROR("input3 is not NULL");
            return SEC_RESULT_FAILURE;
        }

        if (input4 != NULL)
        {
            SEC_LOG_ERROR("input4 is not NULL");
            return SEC_RESULT_FAILURE;
        }

        memcpy(derived.input1, input1, 16);
        memcpy(derived.input2, input2, 16);

        return SecKey_Provision(secProcHandle, object_id_derived, loc_derived, SEC_OPENSSL_KEYCONTAINER_DERIVED, (SEC_BYTE *) &derived, sizeof(derived));
    }

    SEC_LOG_ERROR("Unimplemented root key type %d", root);
    return SEC_RESULT_FAILURE;
}

Sec_KeyType SecKey_GetKeyType(Sec_KeyHandle* keyHandle)
{
    if (keyHandle == NULL)
        return SEC_KEYTYPE_NUM;

    return keyHandle->key_data.info.key_type;
}

Sec_Result SecKey_ComputeBaseKeyDigest(Sec_ProcessorHandle* secProcHandle, SEC_BYTE *nonce,
        Sec_DigestAlgorithm alg, SEC_BYTE *digest, SEC_SIZE *digest_len)
{
    Sec_KeyHandle *base_key = NULL;
    SEC_BYTE base_key_clear[SEC_SYMETRIC_KEY_MAX_LEN];
    SEC_SIZE base_key_len;
    Sec_Result res;

    /* provision base key */
    if (SEC_RESULT_SUCCESS != _Sec_ProvisionBaseKey(secProcHandle, nonce))
    {
        SEC_LOG_ERROR("Could not provision base key");
        return SEC_RESULT_FAILURE;
    }

    if (SEC_RESULT_SUCCESS
            != SecKey_GetInstance(secProcHandle, SEC_OBJECTID_BASE_KEY_MAC,
                    &base_key))
    {
        SEC_LOG_ERROR("SecKey_GetInstance failed");
        return SEC_RESULT_FAILURE;
    }

    base_key_len = SecKey_GetKeyLen(base_key);
    if (SEC_RESULT_SUCCESS != _Sec_SymetricFromKeyHandle(base_key, base_key_clear, base_key_len))
    {
        SEC_LOG_ERROR("_Sec_SymetricFromKeyHandle failed");
        return SEC_RESULT_FAILURE;
    }
    SecKey_Release(base_key);
    base_key = NULL;

    res = SecDigest_SingleInput(secProcHandle, alg, base_key_clear, base_key_len, digest, digest_len);

    Sec_Memset(base_key_clear, 0, sizeof(base_key_clear));

    return res;
}

Sec_ProcessorHandle* SecKey_GetProcessor(Sec_KeyHandle* key)
{
    if (key == NULL)
        return NULL;

    return key->proc;
}

Sec_Result SecBundle_GetInstance(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_BundleHandle **bundleHandle)
{
    Sec_Result result;
    Sec_StorageLoc location;
    _Sec_BundleData bundle_data;

    *bundleHandle = NULL;

    CHECK_HANDLE(secProcHandle);

    if (object_id == SEC_OBJECTID_INVALID)
        return SEC_RESULT_INVALID_PARAMETERS;

    result = _Sec_RetrieveBundleData(secProcHandle, object_id, &location,
            &bundle_data);
    if (result != SEC_RESULT_SUCCESS)
        return result;

    *bundleHandle = calloc(1, sizeof(Sec_BundleHandle));
    if (NULL == *bundleHandle)
    {
        SEC_LOG_ERROR("malloc failed");
        return SEC_RESULT_FAILURE;
    }
    (*bundleHandle)->object_id = object_id;
    memcpy(&((*bundleHandle)->bundle_data), &bundle_data, sizeof(_Sec_BundleData));
    (*bundleHandle)->location = location;
    (*bundleHandle)->proc = secProcHandle;

    return SEC_RESULT_SUCCESS;
}

Sec_Result SecBundle_Provision(Sec_ProcessorHandle* secProcHandle,
        SEC_OBJECTID object_id, Sec_StorageLoc location,
        SEC_BYTE *data, SEC_SIZE data_len)
{
    _Sec_BundleData bundle_data;

    CHECK_HANDLE(secProcHandle);

    if (object_id == SEC_OBJECTID_INVALID)
    {
        SEC_LOG_ERROR("Cannot provision object with SEC_OBJECTID_INVALID");
        return SEC_RESULT_FAILURE;
    }

    if (location == SEC_STORAGELOC_OEM)
    {
        SEC_LOG_ERROR(
                "Cannot provision individual bundles into SEC_STORAGELOC_OEM storage on this platform");
        return SEC_RESULT_FAILURE;
    }

    if (data_len > SEC_BUNDLE_MAX_LEN)
    {
        SEC_LOG_ERROR("Input bundle is too large");
        return SEC_RESULT_FAILURE;
    }

    memcpy(bundle_data.bundle, data, data_len);
    bundle_data.bundle_len = data_len;

    return _Sec_StoreBundleData(secProcHandle, object_id, location, &bundle_data);
}

Sec_Result SecBundle_Delete(Sec_ProcessorHandle* secProcHandle, SEC_OBJECTID object_id)
{
    char file_name[SEC_MAX_FILE_PATH_LEN];
    _Sec_RAMBundleData *ram_bundle = NULL;
    _Sec_RAMBundleData *ram_bundle_parent = NULL;
    SEC_SIZE bundles_found = 0;
    SEC_SIZE bundles_deleted = 0;

    CHECK_HANDLE(secProcHandle);

    /* ram */
    _Sec_FindRAMBundleData(secProcHandle, object_id, &ram_bundle, &ram_bundle_parent);
    if (ram_bundle != NULL)
    {
        if (ram_bundle_parent == NULL)
            secProcHandle->ram_bundles = ram_bundle->next;
        else
            ram_bundle_parent->next = ram_bundle->next;

        Sec_Memset(ram_bundle, 0, sizeof(_Sec_RAMBundleData));

        SEC_FREE(ram_bundle);

        ++bundles_found;
        ++bundles_deleted;
    }

    /* file system */
    snprintf(file_name, sizeof(file_name), "%s" SEC_BUNDLE_FILENAME_PATTERN,
            secProcHandle->bundlestorage_file_dir, object_id);
    if (SecUtils_FileExists(file_name))
    {
        SecUtils_RmFile(file_name);
        ++bundles_found;

        if (!SecUtils_FileExists(file_name))
            ++bundles_deleted;
    }

    if (bundles_found == 0)
        return SEC_RESULT_NO_SUCH_ITEM;

    if (bundles_found != bundles_deleted)
    {
        SEC_LOG_ERROR(
                "Could not delete the specified bundle.  It is stored in a non-removable location.");
        return SEC_RESULT_ITEM_NON_REMOVABLE;
    }

    return SEC_RESULT_SUCCESS;
}


Sec_Result SecBundle_Export(Sec_BundleHandle* bundle_handle,
        SEC_BYTE *buffer, SEC_SIZE buffer_len, SEC_SIZE *written)
{
    CHECK_HANDLE(bundle_handle);

    if (buffer == NULL)
    {
        *written = bundle_handle->bundle_data.bundle_len;
        return SEC_RESULT_SUCCESS;
    }

    if (buffer_len < bundle_handle->bundle_data.bundle_len)
        return SEC_RESULT_BUFFER_TOO_SMALL;

    memcpy(buffer, bundle_handle->bundle_data.bundle,
            bundle_handle->bundle_data.bundle_len);
    *written = bundle_handle->bundle_data.bundle_len;
    return SEC_RESULT_SUCCESS;
}

Sec_Result SecBundle_Release(Sec_BundleHandle* bundleHandle)
{
    CHECK_HANDLE(bundleHandle);

    SEC_FREE(bundleHandle);

    return SEC_RESULT_SUCCESS;
}