// fopen et getenv suffisent ici : pas d'avertissement de MSVC sur leurs variantes « sûres »
#if defined (_MSC_VER) && ! defined (_CRT_SECURE_NO_WARNINGS)
 #define _CRT_SECURE_NO_WARNINGS
#endif

#include "LicenseCore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shellapi.h>
 #include <shlobj.h>
#elif defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
 #include <IOKit/IOKitLib.h>
 #include <spawn.h>
 #include <unistd.h>
 extern char** environ;
#endif

extern "C"
{
#include "tweetnacl.h"

// TweetNaCl réclame cette fonction pour créer des clés et signer. Le plugin ne fait que vérifier :
// elle n'est jamais appelée.
void randombytes (unsigned char* bytes, unsigned long long count)
{
    std::memset (bytes, 0, (size_t) count);
}
}

namespace onirique::license::core
{

// Clé publique du serveur ONIRIQUE CENTRAL (https://central.onirique.store/api/public-key, champ "raw").
// Le serveur refuse de publier un plugin payant dont le binaire ne contient pas ces 32 octets.
static const unsigned char serverPublicKey[32] = {
    0xd0, 0xc1, 0x4c, 0xa4, 0x0e, 0x26, 0xe2, 0xce, 0x06, 0xaf, 0xa9, 0x8f, 0xd0, 0xcb, 0xe6, 0x48,
    0x7d, 0x67, 0x9f, 0x57, 0x55, 0x41, 0xed, 0x75, 0x75, 0x9f, 0x58, 0x9b, 0xb9, 0x0f, 0x0b, 0xac
};

//==============================================================================
//  SHA-256 (FIPS 180-4), pour l'empreinte de la machine.
//==============================================================================
static std::string sha256Hex (const std::string& text)
{
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    std::uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    // Le message, suivi d'un bit à 1, de zéros, puis de sa longueur en bits sur 64 bits.
    std::vector<unsigned char> message (text.begin(), text.end());
    const std::uint64_t bits = (std::uint64_t) message.size() * 8;
    message.push_back (0x80);
    while (message.size() % 64 != 56)
        message.push_back (0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back ((unsigned char) (bits >> shift));

    const auto rotr = [] (std::uint32_t value, int count) { return (value >> count) | (value << (32 - count)); };

    for (size_t block = 0; block < message.size(); block += 64)
    {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            const auto* p = message.data() + block + (size_t) i * 4;
            w[i] = ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8) | (std::uint32_t) p[3];
        }
        for (int i = 16; i < 64; ++i)
        {
            const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const auto s1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const auto t1 = hh + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto s0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const auto t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::string hex;
    for (const auto word : h)
    {
        char text8[9];
        std::snprintf (text8, sizeof (text8), "%08x", (unsigned int) word);
        hex += text8;
    }
    return hex;
}

//==============================================================================
//  Base64 et JSON : juste ce qu'il faut pour un fichier de licence.
//==============================================================================
static bool decodeBase64 (const std::string& text, std::vector<unsigned char>& out)
{
    out.clear();
    std::uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;

    for (const char c : text)
    {
        int value;
        if (c >= 'A' && c <= 'Z')       value = c - 'A';
        else if (c >= 'a' && c <= 'z')  value = c - 'a' + 26;
        else if (c >= '0' && c <= '9')  value = c - '0' + 52;
        else if (c == '+')              value = 62;
        else if (c == '/')              value = 63;
        else if (c == '=')              { ++padding; continue; }
        else                            return false;

        if (padding > 0)
            return false; // rien après le remplissage

        buffer = (buffer << 6) | (std::uint32_t) value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back ((unsigned char) ((buffer >> bits) & 0xff));
        }
    }

    return padding <= 2;
}

struct Field
{
    enum class Kind { text, number, null, other };
    Kind kind = Kind::other;
    std::string text;
    double number = 0.0;
};

using Fields = std::map<std::string, Field>;

static void skipSpaces (const std::string& s, size_t& at)
{
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r'))
        ++at;
}

static void appendUtf8 (std::string& out, std::uint32_t code)
{
    if (code < 0x80)
        out += (char) code;
    else if (code < 0x800)
    {
        out += (char) (0xc0 | (code >> 6));
        out += (char) (0x80 | (code & 0x3f));
    }
    else if (code < 0x10000)
    {
        out += (char) (0xe0 | (code >> 12));
        out += (char) (0x80 | ((code >> 6) & 0x3f));
        out += (char) (0x80 | (code & 0x3f));
    }
    else
    {
        out += (char) (0xf0 | (code >> 18));
        out += (char) (0x80 | ((code >> 12) & 0x3f));
        out += (char) (0x80 | ((code >> 6) & 0x3f));
        out += (char) (0x80 | (code & 0x3f));
    }
}

static bool parseHex4 (const std::string& s, size_t at, std::uint32_t& code)
{
    if (at + 4 > s.size())
        return false;

    code = 0;
    for (size_t i = at; i < at + 4; ++i)
    {
        const char c = s[i];
        code <<= 4;
        if (c >= '0' && c <= '9')       code |= (std::uint32_t) (c - '0');
        else if (c >= 'a' && c <= 'f')  code |= (std::uint32_t) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  code |= (std::uint32_t) (c - 'A' + 10);
        else                            return false;
    }
    return true;
}

static bool parseString (const std::string& s, size_t& at, std::string& out)
{
    if (at >= s.size() || s[at] != '"')
        return false;

    out.clear();
    for (++at; at < s.size(); ++at)
    {
        const char c = s[at];
        if (c == '"')
        {
            ++at;
            return true;
        }
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (++at >= s.size())
            return false;

        switch (s[at])
        {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
            {
                std::uint32_t code = 0, low = 0;
                if (! parseHex4 (s, at + 1, code))
                    return false;
                at += 4;
                // Deux échappements à la suite pour un caractère hors du plan de base.
                if (code >= 0xd800 && code < 0xdc00 && at + 2 < s.size() && s[at + 1] == '\\' && s[at + 2] == 'u'
                    && parseHex4 (s, at + 3, low) && low >= 0xdc00 && low < 0xe000)
                {
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    at += 6;
                }
                appendUtf8 (out, code);
                break;
            }
            default: return false;
        }
    }

    return false; // chaîne non fermée
}

/** Un objet JSON à un seul niveau : chaînes, nombres, null, booléens. Tout le reste est refusé. */
static bool parseObject (const std::string& s, Fields& out)
{
    out.clear();
    size_t at = 0;
    skipSpaces (s, at);
    if (at >= s.size() || s[at] != '{')
        return false;
    ++at;

    skipSpaces (s, at);
    if (at < s.size() && s[at] == '}')
        return true;

    for (;;)
    {
        std::string key;
        skipSpaces (s, at);
        if (! parseString (s, at, key))
            return false;

        skipSpaces (s, at);
        if (at >= s.size() || s[at] != ':')
            return false;
        ++at;
        skipSpaces (s, at);
        if (at >= s.size())
            return false;

        Field field;
        if (s[at] == '"')
        {
            field.kind = Field::Kind::text;
            if (! parseString (s, at, field.text))
                return false;
        }
        else if (s.compare (at, 4, "null") == 0)
        {
            field.kind = Field::Kind::null;
            at += 4;
        }
        else if (s.compare (at, 4, "true") == 0)
            at += 4;
        else if (s.compare (at, 5, "false") == 0)
            at += 5;
        else if (s[at] == '-' || (s[at] >= '0' && s[at] <= '9'))
        {
            const char* start = s.c_str() + at;
            char* end = nullptr;
            field.number = std::strtod (start, &end);
            if (end == start)
                return false;
            field.kind = Field::Kind::number;
            at += (size_t) (end - start);
        }
        else
            return false;

        out[key] = field;

        skipSpaces (s, at);
        if (at >= s.size())
            return false;
        if (s[at] == '}')
            return true;
        if (s[at] != ',')
            return false;
        ++at;
    }
}

static const std::string* textOf (const Fields& fields, const char* key)
{
    const auto found = fields.find (key);
    return found != fields.end() && found->second.kind == Field::Kind::text ? &found->second.text : nullptr;
}

//==============================================================================
int Status::daysLeft() const noexcept
{
    if (expiresAt <= 0)
        return 0;

    const auto remaining = expiresAt - nowMs();
    return remaining <= 0 ? 0 : (int) ((remaining + 86399999) / 86400000);
}

std::int64_t nowMs() noexcept
{
    return (std::int64_t) std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count();
}

#if defined (_WIN32)
static std::string toUtf8 (const std::wstring& wide)
{
    if (wide.empty())
        return {};

    const int size = WideCharToMultiByte (CP_UTF8, 0, wide.data(), (int) wide.size(), nullptr, 0, nullptr, nullptr);
    std::string text ((size_t) size, '\0');
    WideCharToMultiByte (CP_UTF8, 0, wide.data(), (int) wide.size(), text.data(), size, nullptr, nullptr);
    return text;
}

static std::wstring toWide (const std::string& text)
{
    if (text.empty())
        return {};

    const int size = MultiByteToWideChar (CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
    std::wstring wide ((size_t) size, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, text.data(), (int) text.size(), wide.data(), size);
    return wide;
}
#endif

std::string machineId()
{
    std::string guid;

   #if defined (_WIN32)
    wchar_t value[128] = {};
    DWORD size = sizeof (value) - sizeof (wchar_t);
    if (RegGetValueW (HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                      RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &size) == ERROR_SUCCESS)
        guid = toUtf8 (value);
   #elif defined (__APPLE__)
    // L'identifiant matériel du Mac (IOPlatformUUID) : le même que lit l'application ONIRIQUE CENTRAL.
    if (const auto entry = IOServiceGetMatchingService (0, IOServiceMatching ("IOPlatformExpertDevice")))
    {
        if (const auto uuid = (CFStringRef) IORegistryEntryCreateCFProperty (entry, CFSTR ("IOPlatformUUID"), kCFAllocatorDefault, 0))
        {
            char text[128] = {};
            if (CFStringGetCString (uuid, text, sizeof (text), kCFStringEncodingUTF8))
                guid = text;
            CFRelease (uuid);
        }

        IOObjectRelease (entry);
    }
   #endif

    // Sans espaces autour, en minuscules (l'identifiant n'a que des caractères ASCII).
    const auto first = guid.find_first_not_of (" \t\r\n");
    const auto last = guid.find_last_not_of (" \t\r\n");
    if (first == std::string::npos)
        return {};

    guid = guid.substr (first, last - first + 1);
    for (auto& c : guid)
        if (c >= 'A' && c <= 'Z')
            c = (char) (c - 'A' + 'a');

    return sha256Hex ("onirique:" + guid).substr (0, 40);
}

std::string licensePath (const std::string& productSlug)
{
   #if defined (_WIN32)
    wchar_t folder[MAX_PATH] = {};
    if (SHGetFolderPathW (nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, folder) != S_OK)
        return {};

    return toUtf8 (folder) + "\\Onirique\\Licenses\\" + productSlug + ".lic";
   #elif defined (__APPLE__)
    // Dossier commun à tous les comptes du Mac, où l'application ONIRIQUE CENTRAL écrit sans mot de passe.
    return "/Users/Shared/Onirique/Licenses/" + productSlug + ".lic";
   #else
    return {}; // ONIRIQUE CENTRAL n'existe que pour Windows et Mac
   #endif
}

/** Le contenu du fichier, ou rien s'il n'existe pas ou dépasse la taille d'un fichier de licence. */
static std::string readLicenseFile (const std::string& path)
{
    if (path.empty())
        return {};

   #if defined (_WIN32)
    FILE* file = _wfopen (toWide (path).c_str(), L"rb");
   #else
    FILE* file = std::fopen (path.c_str(), "rb");
   #endif
    if (file == nullptr)
        return {};

    // Un fichier de licence fait quelques centaines d'octets : au-delà de 16 Ko, ce n'en est pas un.
    std::string contents (16385, '\0');
    contents.resize (std::fread (contents.data(), 1, contents.size(), file));
    std::fclose (file);
    return contents.size() > 16384 ? std::string() : contents;
}

Status verify (const std::string& fileContents, const std::string& productSlug, const std::string& machine,
               const unsigned char* publicKey, std::int64_t now)
{
    const Status none;

    if (machine.empty() || fileContents.empty() || fileContents.size() > 16384)
        return none;

    Fields envelope;
    if (! parseObject (fileContents, envelope))
        return none;

    const auto* format = textOf (envelope, "format");
    const auto* payloadText = textOf (envelope, "payload");
    const auto* signatureText = textOf (envelope, "signature");
    if (format == nullptr || *format != "onirique-license-v1" || payloadText == nullptr || signatureText == nullptr)
        return none;

    std::vector<unsigned char> payload, signature;
    if (! decodeBase64 (*payloadText, payload) || ! decodeBase64 (*signatureText, signature))
        return none;
    if (signature.size() != crypto_sign_BYTES || payload.empty())
        return none;

    // Vérification Ed25519 : TweetNaCl attend la signature suivie du message, et rend le message s'il est authentique.
    std::vector<unsigned char> signedMessage (signature);
    signedMessage.insert (signedMessage.end(), payload.begin(), payload.end());

    std::vector<unsigned char> opened (signedMessage.size());
    unsigned long long openedSize = 0;
    if (crypto_sign_open (opened.data(), &openedSize, signedMessage.data(), signedMessage.size(), publicKey) != 0)
        return none;

    // À partir d'ici le contenu vient bien du serveur ONIRIQUE. Reste à savoir s'il vaut pour ce plugin, cette machine, aujourd'hui.
    Fields data;
    if (! parseObject (std::string ((const char*) opened.data(), (size_t) openedSize), data))
        return none;

    const auto* product = textOf (data, "product");
    const auto* licensedMachine = textOf (data, "machine");
    const auto* type = textOf (data, "type");
    if (product == nullptr || *product != productSlug || licensedMachine == nullptr || *licensedMachine != machine || type == nullptr)
        return none;

    // expiresAt : un nombre de millisecondes, ou null pour une licence sans limite. Rien d'autre.
    const auto expiry = data.find ("expiresAt");
    if (expiry == data.end() || (expiry->second.kind != Field::Kind::number && expiry->second.kind != Field::Kind::null))
        return none;

    Status status;
    status.expiresAt = expiry->second.kind == Field::Kind::number ? (std::int64_t) expiry->second.number : 0;
    if (expiry->second.kind == Field::Kind::number && status.expiresAt <= now)
        return none;

    if (const auto* email = textOf (data, "email"))
        status.email = *email;

    status.state = *type == "trial" ? State::trial : State::licensed;
    return status;
}

Status read (const std::string& productSlug)
{
    // Pour tester le mode démo sur une machine qui a une licence. Ne peut que retirer la licence, jamais en donner une.
    if (const char* demo = std::getenv ("ONIRIQUE_DEMO"); demo != nullptr && demo[0] != '\0')
        return {};

    return verify (readLicenseFile (licensePath (productSlug)), productSlug, machineId(), serverPublicKey, nowMs());
}

void openCentral()
{
    static const char* downloadPage = "https://onirique.store/#telechargement";

   #if defined (_WIN32)
    // L'application installée pour cet utilisateur ; à défaut, la page de téléchargement.
    wchar_t home[MAX_PATH] = {};
    std::wstring app;
    if (SHGetFolderPathW (nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, home) == S_OK)
        app = std::wstring (home) + L"\\Programs\\ONIRIQUE CENTRAL\\ONIRIQUE CENTRAL.exe";

    const bool installed = ! app.empty() && GetFileAttributesW (app.c_str()) != INVALID_FILE_ATTRIBUTES;
    ShellExecuteW (nullptr, L"open", installed ? app.c_str() : toWide (downloadPage).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
   #elif defined (__APPLE__)
    static const char* app = "/Applications/ONIRIQUE CENTRAL.app";
    const char* target = access (app, F_OK) == 0 ? app : downloadPage;
    char* const arguments[] = { (char*) "open", (char*) target, nullptr };
    pid_t child = 0;
    posix_spawn (&child, "/usr/bin/open", nullptr, nullptr, arguments, environ);
   #else
    (void) downloadPage;
   #endif
}

//==============================================================================
/** La licence d'un plugin, relue toutes les cinq secondes. Une seule par plugin dans le processus,
    partagée par toutes ses instances. */
struct Guard::Watch
{
    explicit Watch (std::string productSlug) : slug (std::move (productSlug))
    {
        refresh();
        thread = std::thread ([this]
        {
            std::unique_lock<std::mutex> lock (waitLock);
            while (! wake.wait_for (lock, std::chrono::seconds (5), [this] { return stopping; }))
            {
                lock.unlock();
                refresh();
                lock.lock();
            }
        });
    }

    ~Watch()
    {
        {
            const std::lock_guard<std::mutex> lock (waitLock);
            stopping = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void refresh()
    {
        const auto fresh = read (slug);
        state.store ((int) fresh.state, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> lock (statusLock);
        status = fresh;
    }

    const std::string slug;
    std::atomic<int> state { (int) State::unlicensed };

    std::mutex statusLock;
    Status status;

    std::mutex waitLock;
    std::condition_variable wake;
    bool stopping = false;
    std::thread thread;
};

Guard::Guard (const std::string& productSlug)
{
    static std::mutex registryLock;
    static std::map<std::string, std::weak_ptr<Watch>> registry;

    const std::lock_guard<std::mutex> lock (registryLock);
    auto& slot = registry[productSlug];
    watch = slot.lock();
    if (watch == nullptr)
    {
        watch = std::make_shared<Watch> (productSlug);
        slot = watch;
    }
}

Status Guard::status() const
{
    const std::lock_guard<std::mutex> lock (watch->statusLock);
    return watch->status;
}

void Guard::refresh()
{
    watch->refresh();
}

State Guard::currentState() const noexcept
{
    return (State) watch->state.load (std::memory_order_relaxed);
}

} // namespace onirique::license::core
