#include "composehooks.h"

#include "albums.h"

namespace compose {

namespace {

PresenceLineFactory& presenceFactory()
{
    static PresenceLineFactory factory; // reset by its owner (PeerHub) at plugin shutdown
    return factory;
}

} // namespace

void setPresenceLineFactory(PresenceLineFactory factory)
{
    presenceFactory() = std::move(factory);
}

PresenceLineFactory presenceLineFactory()
{
    return presenceFactory();
}

QWidget* createPresenceLine(QWidget* parent, const ChatTarget& target)
{
    const PresenceLineFactory& factory = presenceFactory();
    return factory ? factory(parent, target) : nullptr;
}

bool albumsEnabled()
{
    return albums::enabled();
}

} // namespace compose
