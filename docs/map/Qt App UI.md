---
tags: [app, entry]
---

# Qt App UI

**The desktop face of LEXIS**: a Qt Quick (QML) app styled on FluentWinUI3 — group sidebar, document list, and the chat panel where a user's prompt enters the system.

Source: `app/qml/` — `Main.qml`, `GroupSidebar.qml`, `GroupsListView.qml`, `GroupDocumentsView.qml`, `ChatPanel.qml`, `Theme.qml` (a qmldir-registered singleton holding every design token).

- Groups ("corpora") are the unit of organization: each holds its own documents and chat sessions.
- `ChatPanel.qml` sends the typed prompt to [[App Controller]] and renders the streamed-back answer with per-passage source citations.
- Drag-and-drop of PDF/DOCX/TXT/image files starts ingestion via [[App Controller]].

**Next in the prompt flow:** [[App Controller]].
