#include "inputbars.h"

#include <QBoxLayout>
#include <QLayout>
#include <QVector>
#include <QWidget>

#include <functional>

namespace inputbars {

namespace {

bool ours(const QWidget* widget)
{
    return widget && widget->objectName().startsWith(QLatin1String("tsmedia"));
}

struct Step {
    QLayout* layout = nullptr;
    int      index  = 0;
};

// The layouts from holder's down to the one with input in it, and the index taken in each.
bool pathTo(QLayout* layout, QWidget* input, QVector<Step>* path)
{
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (item->widget() == input) {
            path->append({layout, i});
            return true;
        }
        if (QLayout* sub = item->layout()) {
            path->append({layout, i});
            if (pathTo(sub, input, path))
                return true;
            path->removeLast();
        }
    }
    return false;
}

} // namespace

bool isStackBar(const QWidget* widget, const QWidget* input)
{
    return ours(widget) && !widget->isWindow() && widget->isVisible() && !(input && input->isAncestorOf(widget)) && widget != input;
}

bool insertAbove(QWidget* input, QWidget* bar)
{
    if (!input || !bar)
        return false;
    QWidget* holder = input->parentWidget();
    QLayout* root   = holder ? holder->layout() : nullptr;
    QVector<Step> path;
    if (!root || !pathTo(root, input, &path))
        return false;
    for (int k = path.size() - 1; k >= 0; --k) {
        auto* box = qobject_cast<QBoxLayout*>(path.at(k).layout);
        if (!box)
            continue;
        const QBoxLayout::Direction dir = box->direction();
        if (dir != QBoxLayout::TopToBottom && dir != QBoxLayout::BottomToTop)
            continue;
        // Above the input (or the row holding it) and above our bars right on it.
        const int step  = dir == QBoxLayout::TopToBottom ? -1 : 1;
        int       index = path.at(k).index;
        for (int i = index + step; i >= 0 && i < box->count(); i += step) {
            QWidget* w = box->itemAt(i)->widget();
            if (!ours(w) || w == bar)
                break;
            index = i;
        }
        const int existing = box->indexOf(bar);
        if (existing >= 0) { // already in this layout: where it is (the stack's top), unless it moved down
            if ((dir == QBoxLayout::TopToBottom && existing <= index) || (dir == QBoxLayout::BottomToTop && existing >= index))
                return true;
            box->removeWidget(bar);
            if (dir == QBoxLayout::TopToBottom && existing < index)
                --index;
        }
        box->insertWidget(dir == QBoxLayout::TopToBottom ? index : index + 1, bar);
        return true;
    }
    return false;
}

int stackTop(QWidget* input, QWidget* owner, const QWidget* except, QRect* topBar)
{
    if (topBar)
        *topBar = QRect();
    if (!input || !owner)
        return 0;
    const QPoint at    = input->mapTo(owner, QPoint(0, 0));
    const int    left  = at.x();
    const int    right = at.x() + input->width();
    int          top   = at.y();
    // Bars in a layout are children of a widget the input is in; floating ones are children of the window:
    // the direct children of the input's ancestors (not every widget of TeamSpeak's window, 25 times a
    // second while the hold strip shows).
    QVector<QRect> bars;
    for (QWidget* level = input->parentWidget(); level; level = level == owner ? nullptr : level->parentWidget()) {
        for (QObject* child : level->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (!w || w == except || !isStackBar(w, input) || !w->isVisibleTo(owner))
                continue;
            const QRect r(w->mapTo(owner, QPoint(0, 0)), w->size());
            if (r.height() > 0 && r.right() >= left && r.left() < right) // over the input's width
                bars.append(r);
        }
    }
    // Walk up the stack: a bar whose bottom edge is (about) on the current top. 2.3: TeamSpeak's layout may
    // keep its spacing between the bars (the music bar, the reply bar, the input): a gap up to kStackGap.
    constexpr int kStackGap = 8;
    for (bool moved = true; moved;) {
        moved = false;
        for (const QRect& r : qAsConst(bars)) {
            if (r.top() < top && r.bottom() + 1 <= top + 3 && top - (r.bottom() + 1) <= kStackGap) {
                top   = r.top();
                moved = true;
                if (topBar)
                    *topBar = r;
            }
        }
    }
    return top;
}

void placeOverlay(QWidget* input, QWidget* bar, int height)
{
    if (!input || !bar)
        return;
    QWidget* owner = input->window();
    if (bar->parentWidget() != owner) {
        const bool shown = bar->isVisible();
        bar->setParent(owner); // hidden by setParent
        if (shown)
            bar->show();
    }
    const int   top = stackTop(input, owner, bar);
    const QRect place(input->mapTo(owner, QPoint(0, 0)).x(), qMax(0, top - height), input->width(), height);
    if (bar->geometry() != place)
        bar->setGeometry(place);
    const QObjectList& siblings = owner->children();
    if (siblings.isEmpty() || siblings.last() != bar) // over the chat (raise() repaints, so only when needed)
        bar->raise();
}

} // namespace inputbars
