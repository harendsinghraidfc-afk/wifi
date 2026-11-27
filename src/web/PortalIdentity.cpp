#include "web/PortalIdentity.h"
#include <LittleFS.h>
#include <esp_random.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

static int randomBytes(void*, unsigned char* out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}

bool PortalIdentity::loadOrCreate(IPAddress address) {
    auto file = LittleFS.open("/tls-identity", "r");
    if (file && file.size() == sizeof(*this) &&
        file.read(reinterpret_cast<uint8_t*>(this), sizeof(*this)) == sizeof(*this) &&
        ip == uint32_t(address) && !cert[sizeof(cert)-1] && !key[sizeof(key)-1] &&
        strstr(cert, "-----END CERTIFICATE-----") && strstr(key, "PRIVATE KEY-----")) return true;
    file.close();
    memset(this, 0, sizeof(*this));
    ip = uint32_t(address);
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x3f) | 1;
    const unsigned char san[] = {0x30,6,0x87,4,address[0],address[1],address[2],address[3]};
    const char* subject = "CN=Lesh local portal";
    bool ok = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), randomBytes, nullptr) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&crt, subject) == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&crt, subject) == 0 &&
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0 &&
        mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000") == 0 &&
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0 &&
        mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0 &&
        mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, sizeof(san)) == 0 &&
        mbedtls_x509write_crt_pem(&crt, reinterpret_cast<unsigned char*>(cert), sizeof(cert), randomBytes, nullptr) == 0 &&
        mbedtls_pk_write_key_pem(&pk, reinterpret_cast<unsigned char*>(key), sizeof(key)) == 0;
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    if (!ok) return false;
    file = LittleFS.open("/tls-identity.tmp", "w");
    ok = file && file.write(reinterpret_cast<const uint8_t*>(this), sizeof(*this)) == sizeof(*this);
    file.close();
    return ok && LittleFS.rename("/tls-identity.tmp", "/tls-identity");
}
