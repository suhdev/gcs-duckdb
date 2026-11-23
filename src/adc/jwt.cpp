#include "adc/jwt.hpp"

#include "adc/encoding.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace duckdb {
namespace gcs_adc {

namespace {
struct BioDeleter {
	void operator()(BIO *bio) const {
		BIO_free(bio);
	}
};
struct PKeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
struct MdCtxDeleter {
	void operator()(EVP_MD_CTX *ctx) const {
		EVP_MD_CTX_free(ctx);
	}
};
} // namespace

string SignRS256(const string &pem_private_key, const string &data) {
	std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(pem_private_key.data(), int(pem_private_key.size())));
	if (!bio) {
		throw IOException("gcs_adc: could not allocate an OpenSSL buffer for the service account key");
	}
	std::unique_ptr<EVP_PKEY, PKeyDeleter> key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
	if (!key) {
		ERR_clear_error();
		throw InvalidInputException("gcs_adc: the service account 'private_key' is not a valid PEM private key");
	}
	if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
		throw InvalidInputException("gcs_adc: the service account 'private_key' is not an RSA key");
	}
	std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
	if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
	    EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
		ERR_clear_error();
		throw IOException("gcs_adc: failed to initialize RS256 signing");
	}
	size_t signature_length = 0;
	if (EVP_DigestSignFinal(ctx.get(), nullptr, &signature_length) != 1) {
		ERR_clear_error();
		throw IOException("gcs_adc: failed to compute the RS256 signature length");
	}
	string signature(signature_length, '\0');
	if (EVP_DigestSignFinal(ctx.get(), reinterpret_cast<unsigned char *>(&signature[0]), &signature_length) != 1) {
		ERR_clear_error();
		throw IOException("gcs_adc: failed to compute the RS256 signature");
	}
	signature.resize(signature_length);
	return signature;
}

string CreateSignedJwt(const string &pem_private_key, const string &header_json, const string &claims_json) {
	auto signing_input = Base64UrlEncode(header_json) + "." + Base64UrlEncode(claims_json);
	auto signature = SignRS256(pem_private_key, signing_input);
	return signing_input + "." + Base64UrlEncode(signature);
}

} // namespace gcs_adc
} // namespace duckdb
