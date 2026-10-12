#include "License.h"

namespace onirique::license
{

int Status::daysLeft() const noexcept
{
    core::Status status;
    status.expiresAt = expiresAt;
    return status.daysLeft();
}

juce::String machineId()
{
    return juce::String::fromUTF8 (core::machineId().c_str());
}

juce::File licenseFile (const juce::String& productSlug)
{
    return juce::File (juce::String::fromUTF8 (core::licensePath (productSlug.toStdString()).c_str()));
}

Status read (const juce::String& productSlug)
{
    return core::read (productSlug.toStdString());
}

Status verify (const juce::File& file, const juce::String& productSlug, const juce::String& machine,
               const unsigned char* publicKey, juce::int64 nowMs)
{
    // Un fichier de licence fait quelques centaines d'octets : au-delà, ce n'en est pas un.
    if (! file.existsAsFile() || file.getSize() > 16384)
        return {};

    juce::MemoryBlock contents;
    file.loadFileAsData (contents);
    return core::verify (std::string ((const char*) contents.getData(), contents.getSize()),
                         productSlug.toStdString(), machine.toStdString(), publicKey, (std::int64_t) nowMs);
}

} // namespace onirique::license
