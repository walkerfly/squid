/*
 * Copyright (C) 1996-2015 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "base/Packable.h"
#include "Debug.h"
#include "fatal.h"
#include "globals.h"
#include "parser/Tokenizer.h"
#include "parser/Tokenizer.h"
#include "Parsing.h"
#include "security/PeerOptions.h"

#if USE_OPENSSL
#include "ssl/support.h"
#endif

Security::PeerOptions Security::ProxyOutgoingConfig;

Security::PeerOptions::PeerOptions(const Security::PeerOptions &p) :
    sslOptions(p.sslOptions),
    caDir(p.caDir),
    crlFile(p.crlFile),
    sslCipher(p.sslCipher),
    sslFlags(p.sslFlags),
    sslDomain(p.sslDomain),
    parsedOptions(p.parsedOptions),
    parsedFlags(p.parsedFlags),
    certs(p.certs),
    caFiles(p.caFiles),
    parsedCrl(p.parsedCrl),
    sslVersion(p.sslVersion),
    encryptTransport(p.encryptTransport)
{
    memcpy(&flags, &p.flags, sizeof(flags));
}

void
Security::PeerOptions::parse(const char *token)
{
    if (!*token) {
        // config says just "ssl" or "tls" (or "tls-")
        encryptTransport = true;
        return;
    }

    if (strncmp(token, "disable", 7) == 0) {
        clear();
        return;
    }

    if (strncmp(token, "cert=", 5) == 0) {
        KeyData t;
        t.privateKeyFile = t.certFile = SBuf(token + 5);
        certs.emplace_back(t);
    } else if (strncmp(token, "key=", 4) == 0) {
        if (certs.empty() || certs.back().certFile.isEmpty()) {
            debugs(3, DBG_PARSE_NOTE(1), "ERROR: cert= option must be set before key= is used.");
            return;
        }
        KeyData &t = certs.back();
        t.privateKeyFile = SBuf(token + 4);
    } else if (strncmp(token, "version=", 8) == 0) {
        debugs(0, DBG_PARSE_NOTE(1), "UPGRADE WARNING: SSL version= is deprecated. Use options= to limit protocols instead.");
        sslVersion = xatoi(token + 8);
    } else if (strncmp(token, "min-version=", 12) == 0) {
        tlsMinVersion = SBuf(token + 12);
    } else if (strncmp(token, "options=", 8) == 0) {
        sslOptions = SBuf(token + 8);
        parsedOptions = parseOptions();
    } else if (strncmp(token, "cipher=", 7) == 0) {
        sslCipher = SBuf(token + 7);
    } else if (strncmp(token, "cafile=", 7) == 0) {
        caFiles.emplace_back(SBuf(token + 7));
    } else if (strncmp(token, "capath=", 7) == 0) {
        caDir = SBuf(token + 7);
#if !USE_OPENSSL
        debugs(3, DBG_PARSE_NOTE(1), "WARNING: capath= option requires --with-openssl.");
#endif
    } else if (strncmp(token, "crlfile=", 8) == 0) {
        crlFile = SBuf(token + 8);
        loadCrlFile();
    } else if (strncmp(token, "flags=", 6) == 0) {
        if (parsedFlags != 0) {
            debugs(3, DBG_PARSE_NOTE(1), "WARNING: Overwriting flags=" << sslFlags << " with " << SBuf(token + 6));
        }
        sslFlags = SBuf(token + 6);
        parsedFlags = parseFlags();
    } else if (strncmp(token, "no-default-ca", 13) == 0) {
        flags.tlsDefaultCa = false;
    } else if (strncmp(token, "domain=", 7) == 0) {
        sslDomain = SBuf(token + 7);
    } else if (strncmp(token, "no-npn", 6) == 0) {
        flags.tlsNpn = false;
    } else {
        debugs(3, DBG_CRITICAL, "ERROR: Unknown TLS option '" << token << "'");
        return;
    }

    encryptTransport = true;
}

void
Security::PeerOptions::dumpCfg(Packable *p, const char *pfx) const
{
    if (!encryptTransport) {
        p->appendf(" %sdisable", pfx);
        return; // no other settings are relevant
    }

    for (auto &i : certs) {
        if (!i.certFile.isEmpty())
            p->appendf(" %scert=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(i.certFile));

        if (!i.privateKeyFile.isEmpty() && i.privateKeyFile != i.certFile)
            p->appendf(" %skey=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(i.privateKeyFile));
    }

    if (!sslOptions.isEmpty())
        p->appendf(" %soptions=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(sslOptions));

    if (!sslCipher.isEmpty())
        p->appendf(" %scipher=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(sslCipher));

    for (auto i : caFiles) {
        p->appendf(" %scafile=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(i));
    }

    if (!caDir.isEmpty())
        p->appendf(" %scapath=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(caDir));

    if (!crlFile.isEmpty())
        p->appendf(" %scrlfile=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(crlFile));

    if (!sslFlags.isEmpty())
        p->appendf(" %sflags=" SQUIDSBUFPH, pfx, SQUIDSBUFPRINT(sslFlags));

    if (!flags.tlsDefaultCa)
        p->appendf(" %sno-default-ca", pfx);

    if (!flags.tlsNpn)
        p->appendf(" %sno-npn", pfx);
}

void
Security::PeerOptions::updateTlsVersionLimits()
{
    if (!tlsMinVersion.isEmpty()) {
        ::Parser::Tokenizer tok(tlsMinVersion);
        int64_t v = 0;
        if (tok.skip('1') && tok.skip('.') && tok.int64(v, 10, false, 1) && v <= 3) {
            // only account for TLS here - SSL versions are handled by options= parameter
            // avoid affecting options= parameter in cachemgr config report
#if SSL_OP_NO_TLSv1
            if (v > 0)
                parsedOptions |= SSL_OP_NO_TLSv1;
#endif
#if SSL_OP_NO_TLSv1_1
            if (v > 1)
                parsedOptions |= SSL_OP_NO_TLSv1_1;
#endif
#if SSL_OP_NO_TLSv1_2
            if (v > 2)
                parsedOptions |= SSL_OP_NO_TLSv1_2;
#endif

        } else {
            debugs(0, DBG_PARSE_NOTE(1), "WARNING: Unknown TLS minimum version: " << tlsMinVersion);
        }

    } else if (sslVersion > 2) {
        // backward compatibility hack for sslversion= configuration
        // only use if tls-min-version=N.N is not present
        // values 0-2 for auto and SSLv2 are not supported any longer.
        // Do it this way so we DO cause changes to options= in cachemgr config report
        const char *add = NULL;
        switch (sslVersion) {
        case 3:
            add = "NO_TLSv1,NO_TLSv1_1,NO_TLSv1_2";
            break;
        case 4:
            add = "NO_SSLv3,NO_TLSv1_1,NO_TLSv1_2";
            break;
        case 5:
            add = "NO_SSLv3,NO_TLSv1,NO_TLSv1_2";
            break;
        case 6:
            add = "NO_SSLv3,NO_TLSv1,NO_TLSv1_1";
            break;
        default: // nothing
            break;
        }
        if (add) {
            if (!sslOptions.isEmpty())
                sslOptions.append(",",1);
            sslOptions.append(add, strlen(add));
        }
        sslVersion = 0; // prevent sslOptions being repeatedly appended
    }
}

Security::ContextPtr
Security::PeerOptions::createBlankContext() const
{
    Security::ContextPtr t = nullptr;

#if USE_OPENSSL
    Ssl::Initialize();

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
    t = SSL_CTX_new(TLS_client_method());
#else
    t = SSL_CTX_new(SSLv23_client_method());
#endif
    if (!t) {
        const auto x = ERR_error_string(ERR_get_error(), nullptr);
        fatalf("Failed to allocate TLS client context: %s\n", x);
    }

#elif USE_GNUTLS
    // Initialize for X.509 certificate exchange
    if (const int x = gnutls_certificate_allocate_credentials(&t)) {
        fatalf("Failed to allocate TLS client context: error=%d\n", x);
    }

#else
    fatal("Failed to allocate TLS client context: No TLS library\n");

#endif

    return t;
}

Security::ContextPtr
Security::PeerOptions::createClientContext(bool setOptions)
{
    Security::ContextPtr t = nullptr;

    updateTlsVersionLimits();

#if USE_OPENSSL
    // XXX: temporary performance regression. c_str() data copies and prevents this being a const method
    t = sslCreateClientContext(*this, (setOptions ? parsedOptions : 0), parsedFlags);

#elif USE_GNUTLS && WHEN_READY_FOR_GNUTLS
    t = createBlankContext();

#endif

    if (t) {
        updateContextNpn(t);
        updateContextCa(t);
        updateContextCrl(t);
    }

    return t;
}

/// set of options we can parse and what they map to
static struct ssl_option {
    const char *name;
    long value;

} ssl_options[] = {

#if SSL_OP_NETSCAPE_REUSE_CIPHER_CHANGE_BUG
    {
        "NETSCAPE_REUSE_CIPHER_CHANGE_BUG", SSL_OP_NETSCAPE_REUSE_CIPHER_CHANGE_BUG
    },
#endif
#if SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG
    {
        "SSLREF2_REUSE_CERT_TYPE_BUG", SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG
    },
#endif
#if SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER
    {
        "MICROSOFT_BIG_SSLV3_BUFFER", SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER
    },
#endif
#if SSL_OP_SSLEAY_080_CLIENT_DH_BUG
    {
        "SSLEAY_080_CLIENT_DH_BUG", SSL_OP_SSLEAY_080_CLIENT_DH_BUG
    },
#endif
#if SSL_OP_TLS_D5_BUG
    {
        "TLS_D5_BUG", SSL_OP_TLS_D5_BUG
    },
#endif
#if SSL_OP_TLS_BLOCK_PADDING_BUG
    {
        "TLS_BLOCK_PADDING_BUG", SSL_OP_TLS_BLOCK_PADDING_BUG
    },
#endif
#if SSL_OP_TLS_ROLLBACK_BUG
    {
        "TLS_ROLLBACK_BUG", SSL_OP_TLS_ROLLBACK_BUG
    },
#endif
#if SSL_OP_ALL
    {
        "ALL", (long)SSL_OP_ALL
    },
#endif
#if SSL_OP_SINGLE_DH_USE
    {
        "SINGLE_DH_USE", SSL_OP_SINGLE_DH_USE
    },
#endif
#if SSL_OP_EPHEMERAL_RSA
    {
        "EPHEMERAL_RSA", SSL_OP_EPHEMERAL_RSA
    },
#endif
#if SSL_OP_PKCS1_CHECK_1
    {
        "PKCS1_CHECK_1", SSL_OP_PKCS1_CHECK_1
    },
#endif
#if SSL_OP_PKCS1_CHECK_2
    {
        "PKCS1_CHECK_2", SSL_OP_PKCS1_CHECK_2
    },
#endif
#if SSL_OP_NETSCAPE_CA_DN_BUG
    {
        "NETSCAPE_CA_DN_BUG", SSL_OP_NETSCAPE_CA_DN_BUG
    },
#endif
#if SSL_OP_NON_EXPORT_FIRST
    {
        "NON_EXPORT_FIRST", SSL_OP_NON_EXPORT_FIRST
    },
#endif
#if SSL_OP_CIPHER_SERVER_PREFERENCE
    {
        "CIPHER_SERVER_PREFERENCE", SSL_OP_CIPHER_SERVER_PREFERENCE
    },
#endif
#if SSL_OP_NETSCAPE_DEMO_CIPHER_CHANGE_BUG
    {
        "NETSCAPE_DEMO_CIPHER_CHANGE_BUG", SSL_OP_NETSCAPE_DEMO_CIPHER_CHANGE_BUG
    },
#endif
#if SSL_OP_NO_SSLv3
    {
        "NO_SSLv3", SSL_OP_NO_SSLv3
    },
#endif
#if SSL_OP_NO_TLSv1
    {
        "NO_TLSv1", SSL_OP_NO_TLSv1
    },
#endif
#if SSL_OP_NO_TLSv1_1
    {
        "NO_TLSv1_1", SSL_OP_NO_TLSv1_1
    },
#endif
#if SSL_OP_NO_TLSv1_2
    {
        "NO_TLSv1_2", SSL_OP_NO_TLSv1_2
    },
#endif
#if SSL_OP_NO_COMPRESSION
    {
        "No_Compression", SSL_OP_NO_COMPRESSION
    },
#endif
#if SSL_OP_NO_TICKET
    {
        "NO_TICKET", SSL_OP_NO_TICKET
    },
#endif
#if SSL_OP_SINGLE_ECDH_USE
    {
        "SINGLE_ECDH_USE", SSL_OP_SINGLE_ECDH_USE
    },
#endif
    {
        "", 0
    },
    {
        NULL, 0
    }
};

/**
 * Pre-parse TLS options= parameter to be applied when the TLS objects created.
 * Options must not used in the case of peek or stare bump mode.
 */
long
Security::PeerOptions::parseOptions()
{
    long op = 0;
    ::Parser::Tokenizer tok(sslOptions);

    do {
        enum {
            MODE_ADD, MODE_REMOVE
        } mode;

        if (tok.skip('-') || tok.skip('!'))
            mode = MODE_REMOVE;
        else {
            (void)tok.skip('+'); // default action is add. ignore if missing operator
            mode = MODE_ADD;
        }

        static const CharacterSet optChars = CharacterSet("TLS-option", "_") + CharacterSet::ALPHA + CharacterSet::DIGIT;
        int64_t hex = 0;
        SBuf option;
        long value = 0;

        if (tok.int64(hex, 16, false)) {
            /* Special case.. hex specification */
            value = hex;
        }

        else if (tok.prefix(option, optChars)) {
            // find the named option in our supported set
            for (struct ssl_option *opttmp = ssl_options; opttmp->name; ++opttmp) {
                if (option.cmp(opttmp->name) == 0) {
                    value = opttmp->value;
                    break;
                }
            }
        }

        if (value) {
            switch (mode) {
            case MODE_ADD:
                op |= value;
                break;
            case MODE_REMOVE:
                op &= ~value;
                break;
            }
        } else {
            debugs(83, DBG_PARSE_NOTE(1), "ERROR: Unknown TLS option " << option);
        }

        static const CharacterSet delims("TLS-option-delim",":,");
        if (!tok.skipAll(delims) && !tok.atEnd()) {
            fatalf("Unknown TLS option '" SQUIDSBUFPH "'", SQUIDSBUFPRINT(tok.remaining()));
        }

    } while (!tok.atEnd());

#if SSL_OP_NO_SSLv2
    // compliance with RFC 6176: Prohibiting Secure Sockets Layer (SSL) Version 2.0
    op = op | SSL_OP_NO_SSLv2;
#endif
    return op;
}

/**
 * Parses the TLS flags squid.conf parameter
 */
long
Security::PeerOptions::parseFlags()
{
    if (sslFlags.isEmpty())
        return 0;

    static struct {
        SBuf label;
        long mask;
    } flagTokens[] = {
        { SBuf("NO_DEFAULT_CA"), SSL_FLAG_NO_DEFAULT_CA },
        { SBuf("DELAYED_AUTH"), SSL_FLAG_DELAYED_AUTH },
        { SBuf("DONT_VERIFY_PEER"), SSL_FLAG_DONT_VERIFY_PEER },
        { SBuf("DONT_VERIFY_DOMAIN"), SSL_FLAG_DONT_VERIFY_DOMAIN },
        { SBuf("NO_SESSION_REUSE"), SSL_FLAG_NO_SESSION_REUSE },
#if X509_V_FLAG_CRL_CHECK
        { SBuf("VERIFY_CRL"), SSL_FLAG_VERIFY_CRL },
        { SBuf("VERIFY_CRL_ALL"), SSL_FLAG_VERIFY_CRL_ALL },
#endif
        { SBuf(), 0 }
    };

    ::Parser::Tokenizer tok(sslFlags);
    static const CharacterSet delims("Flag-delimiter", ":,");

    long fl = 0;
    do {
        long found = 0;
        for (size_t i = 0; flagTokens[i].mask; ++i) {
            if (tok.skip(flagTokens[i].label) == 0) {
                found = flagTokens[i].mask;
                break;
            }
        }
        if (!found)
            fatalf("Unknown TLS flag '" SQUIDSBUFPH "'", SQUIDSBUFPRINT(tok.remaining()));
        if (found == SSL_FLAG_NO_DEFAULT_CA) {
            debugs(83, DBG_PARSE_NOTE(2), "UPGRADE WARNING: flags=NO_DEFAULT_CA is deprecated. Use tls-no-default-ca instead.");
            flags.tlsDefaultCa = false;
        } else
            fl |= found;
    } while (tok.skipOne(delims));

    return fl;
}

/// Load a CRLs list stored in the file whose /path/name is in crlFile
/// replaces any CRL loaded previously
void
Security::PeerOptions::loadCrlFile()
{
    parsedCrl.clear();
    if (crlFile.isEmpty())
        return;

#if USE_OPENSSL
    BIO *in = BIO_new_file(crlFile.c_str(), "r");
    if (!in) {
        debugs(83, 2, "WARNING: Failed to open CRL file " << crlFile);
        return;
    }

    while (X509_CRL *crl = PEM_read_bio_X509_CRL(in,NULL,NULL,NULL)) {
        parsedCrl.emplace_back(Security::CrlPointer(crl));
    }
    BIO_free(in);
#endif
}

#if USE_OPENSSL && defined(TLSEXT_TYPE_next_proto_neg)
// Dummy next_proto_neg callback
static int
ssl_next_proto_cb(SSL *s, unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *arg)
{
    static const unsigned char supported_protos[] = {8, 'h','t','t', 'p', '/', '1', '.', '1'};
    (void)SSL_select_next_proto(out, outlen, in, inlen, supported_protos, sizeof(supported_protos));
    return SSL_TLSEXT_ERR_OK;
}
#endif

void
Security::PeerOptions::updateContextNpn(Security::ContextPtr &ctx)
{
    if (!flags.tlsNpn)
        return;

#if USE_OPENSSL && defined(TLSEXT_TYPE_next_proto_neg)
    SSL_CTX_set_next_proto_select_cb(ctx, &ssl_next_proto_cb, nullptr);
#endif

    // NOTE: GnuTLS does not support the obsolete NPN extension.
    //       it does support ALPN per-session, not per-context.
}

void
Security::PeerOptions::updateContextCa(Security::ContextPtr &ctx)
{
    debugs(83, 8, "Setting CA certificate locations.");

    for (auto i : caFiles) {
#if USE_OPENSSL
        if (!SSL_CTX_load_verify_locations(ctx, i.c_str(), caDir.c_str())) {
            const int ssl_error = ERR_get_error();
            debugs(83, DBG_IMPORTANT, "WARNING: Ignoring error setting CA certificate locations: " << ERR_error_string(ssl_error, NULL));
        }
#elif USE_GNUTLS
        if (gnutls_certificate_set_x509_trust_file(ctx, i.c_str(), GNUTLS_X509_FMT_PEM) < 0) {
            debugs(83, DBG_IMPORTANT, "WARNING: Ignoring error setting CA certificate location: " << i);
        }
#endif
    }

    if (!flags.tlsDefaultCa)
        return;

#if USE_OPENSSL
    if (!SSL_CTX_set_default_verify_paths(ctx)) {
        const int ssl_error = ERR_get_error();
        debugs(83, DBG_IMPORTANT, "WARNING: Ignoring error setting default trusted CA : "
               << ERR_error_string(ssl_error, NULL));
    }
#elif USE_GNUTLS
    if (gnutls_certificate_set_x509_system_trust(ctx) != GNUTLS_E_SUCCESS) {
        debugs(83, DBG_IMPORTANT, "WARNING: Ignoring error setting default trusted CA.");
    }
#endif
}

void
Security::PeerOptions::updateContextCrl(Security::ContextPtr &ctx)
{
#if USE_OPENSSL
    bool verifyCrl = false;
    X509_STORE *st = SSL_CTX_get_cert_store(ctx);
    if (parsedCrl.size()) {
        for (auto &i : parsedCrl) {
            if (!X509_STORE_add_crl(st, i.get()))
                debugs(83, 2, "WARNING: Failed to add CRL");
            else
                verifyCrl = true;
        }
    }

#if X509_V_FLAG_CRL_CHECK
    if ((parsedFlags & SSL_FLAG_VERIFY_CRL_ALL))
        X509_STORE_set_flags(st, X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL);
    else if (verifyCrl || (parsedFlags & SSL_FLAG_VERIFY_CRL))
        X509_STORE_set_flags(st, X509_V_FLAG_CRL_CHECK);
#endif

#endif /* USE_OPENSSL */
}

void
parse_securePeerOptions(Security::PeerOptions *opt)
{
    while(const char *token = ConfigParser::NextToken())
        opt->parse(token);
}

