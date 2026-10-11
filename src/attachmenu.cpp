#include "attachmenu.h"

#include <algorithm>

namespace attachmenu {

namespace {

QList<Entry>& registry()
{
    static QList<Entry> list;
    return list;
}

} // namespace

void addEntry(const Entry& entry)
{
    if (entry.id.isEmpty())
        return;
    removeEntry(entry.id);
    registry().append(entry);
}

void removeEntry(const QString& id)
{
    QList<Entry>& list = registry();
    for (int i = list.size() - 1; i >= 0; --i) {
        if (list.at(i).id == id)
            list.removeAt(i);
    }
}

QList<Entry> entries()
{
    QList<Entry> out = registry();
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.order != b.order)
            return a.order < b.order;
        return a.id < b.id;
    });
    return out;
}

} // namespace attachmenu
