#include "composehooks.h"

#include <QMimeData>

namespace compose {

namespace {

#ifdef TSMEDIA_ALBUMS
bool g_albums = true;
#else
bool g_albums = false;
#endif

PresenceLineFactory& presenceFactory()
{
    static PresenceLineFactory factory;
    return factory;
}

} // namespace

void setPresenceLineFactory(PresenceLineFactory factory)
{
    presenceFactory() = std::move(factory);
}

QWidget* createPresenceLine(QWidget* parent, const ChatTarget& target)
{
    const PresenceLineFactory& factory = presenceFactory();
    return factory ? factory(parent, target) : nullptr;
}

void setAlbumsEnabled(bool enabled)
{
    g_albums = enabled;
}

bool albumsEnabled()
{
    return g_albums;
}

QString ownDragMimeFormat()
{
    return QString::fromLatin1("application/x-tsmedia-key");
}

bool isOwnDrag(const QMimeData* mime)
{
    return mime && mime->hasFormat(ownDragMimeFormat());
}

} // namespace compose
