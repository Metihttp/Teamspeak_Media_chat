#include "composehooks.h"

namespace compose {

namespace {
PresenceLineFactory& factory()
{
    static PresenceLineFactory instance; // reset by its owner at plugin shutdown
    return instance;
}
} // namespace

void setPresenceLineFactory(PresenceLineFactory f)
{
    factory() = std::move(f);
}

PresenceLineFactory presenceLineFactory()
{
    return factory();
}

} // namespace compose
