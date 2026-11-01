/*
 * Qt 6.12 on macOS installs forwarding headers. The real qgraphicsitem.h,
 * which declares QGraphicsItem as an interface, is reached with include_next,
 * and moc does not follow that. Q_INTERFACES(QGraphicsItem) then fails the
 * build with "Undefined interface". Register the same interface id for moc.
 * The compiler still uses the declaration in Qt's header.
 */
#ifdef Q_MOC_RUN
Q_DECLARE_INTERFACE(QGraphicsItem, "org.qt-project.Qt.QGraphicsItem")
#endif
