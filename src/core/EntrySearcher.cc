/*
 *  Copyright (C) 2008  Alexandre Courbot
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "core/Tag.h"
#include "core/RelativeDate.h"
#include "core/EntrySearcher.h"

#include <QtDebug>

#include <QRegExp>
#include <QStringList>

EntrySearcher::EntrySearcher(QObject *parent) : QObject(parent), commandMatch(SearchCommand::commandMatch().pattern())
{
	QueryBuilder::Join::addTablePriority("training", -100);
	QueryBuilder::Join::addTablePriority("notes", -40);
	QueryBuilder::Join::addTablePriority("notesText", -45);
	QueryBuilder::Join::addTablePriority("taggedEntries", -50);
	QueryBuilder::Join::addTablePriority("tags", -55);
	validCommands << "study" << "nostudy" << "note" << "lasttrained" << "mistaken" << "tag" << "score";
}

SearchCommand EntrySearcher::commandFromWord(const QString &word) const
{
	// We do not recognize any word in this class
	return SearchCommand::invalid();
}

QPair<QDate, QDate> timeInterval(const QString &string, const QString &string2)
{
	RelativeDate rDate(string);
	RelativeDate rDate2(string2);
	QDate time(rDate.date());
	QDate timeMax(rDate2.date());
	return QPair<QDate, QDate>(time, timeMax);
}

void EntrySearcher::buildStatement(QList<SearchCommand> &commands, QueryBuilder::Statement &statement)
{
	QStringList notesSearch;
	QStringList tagSearch;
	foreach(const SearchCommand &command, commands) {
		bool processed = true;
		if (command.command() == "study") {
			if (command.args().size() > 2) continue;
			statement.setFirstTable("training");
			statement.addWhere(QString("training.dateAdded not null"));

			QString s1, s2;
			if (command.args().size() >= 1) s1 = command.args()[0];
			if (command.args().size() >= 2) s2 = command.args()[1];
			QPair<QDate, QDate> interval = timeInterval(s1, s2);
			if (interval.first.isValid()) statement.addWhere(QString("training.dateAdded >= %1").arg(QDateTime(interval.first).toTime_t()));
			if (interval.second.isValid()) statement.addWhere(QString("training.dateAdded < %1").arg(QDateTime(interval.second).toTime_t()));
		}
		else if (command.command() == "nostudy")
			statement.addWhere(QString("training.dateAdded is null"));
		else if (command.command() == "score") {
			if (command.args().size() != 2 && command.args().size() != 1) continue;
			bool ok;
			int from = command.args()[0].toInt(&ok);
			if (!ok) continue;
                        int to = 100;
			if (command.args().size() == 2) {
				to = command.args()[1].toInt(&ok);
				if (!ok) continue;
			}
			if (command.args().size() == 1) statement.addWhere(QString("training.score = %1").arg(from));
			else statement.addWhere(QString("training.score between %1 and %2").arg(from).arg(to));
		}
		else if (command.command() == "note") {
			statement.addJoin(QueryBuilder::Join(QueryBuilder::Column("notes", "id"), QString("notes.type = %1").arg(entryType()), QueryBuilder::Join::Left));
			if (command.args().isEmpty()) statement.addWhere(QString("notes.dateAdded not null"));
			else foreach(const QString &arg, command.args()) notesSearch << "\"" + arg + "\"";
			statement.setFirstTable("notes");
		}
		else if (command.command() == "tag") {
			bool allTagsHandled = false;
			statement.addJoin(QueryBuilder::Join(QueryBuilder::Column("taggedEntries", "id"), QString("taggedEntries.type = %1").arg(entryType()), QueryBuilder::Join::Left));
			if (command.args().isEmpty()) { statement.addWhere(QString("taggedEntries.date not null")); allTagsHandled = true; }
			else foreach(const QString &arg, command.args())
				// We filter the "*" tag as FTS3 does not support it - but since the condition is added, all non-tagged entries will be filtered which is the desired result anyway.
				if (arg != "*") tagSearch << "\"" + arg + "\"";
				else if (!allTagsHandled) { statement.addWhere(QString("taggedEntries.date not null")); allTagsHandled = true; }
			statement.setFirstTable("taggedEntries");
		}
		else if (command.command() == "lasttrained") {
			if (command.args().size() > 2) continue;

			QString s1, s2;
			if (command.args().size() >= 1) s1 = command.args()[0];
			if (command.args().size() >= 2) s2 = command.args()[1];
			QPair<QDate, QDate> interval = timeInterval(s1, s2);
			if (interval.first.isValid()) statement.addWhere(QString("training.dateLastTrain >= %1").arg(QDateTime(interval.first).toTime_t()));
			if (interval.second.isValid()) statement.addWhere(QString("(training.dateLastTrain < %1 or training.dateLastTrain is null)").arg(QDateTime(interval.second).toTime_t()));
			if (!interval.first.isValid() && !interval.second.isValid()) statement.addWhere(QString("training.dateLastTrain not null"));
		}
		else if (command.command() == "mistaken") {
			if (command.args().size() > 2) continue;

			QString s1, s2;
			if (command.args().size() >= 1) s1 = command.args()[0];
			if (command.args().size() >= 2) s2 = command.args()[1];
			QPair<QDate, QDate> interval = timeInterval(s1, s2);
			if (interval.first.isValid()) statement.addWhere(QString("training.dateLastMistake >= %1").arg(QDateTime(interval.first).toTime_t()));
			if (interval.second.isValid()) statement.addWhere(QString("training.dateLastMistake < %1").arg(QDateTime(interval.second).toTime_t()));
			if (!interval.first.isValid() && !interval.second.isValid()) statement.addWhere(QString("training.dateLastMistake not null"));
		}
		else processed = false;
		if (processed) commands.removeOne(command);
	}
	if (!notesSearch.isEmpty()) {
		statement.addWhere(QString("notes.noteId in (select docid from notesText where note match '%1')").arg(notesSearch.join(" ")));
	}
	if (!tagSearch.isEmpty()) {
		// Remove duplicates, case insensitively
		QSet<QString> tmpSet;
		foreach (const QString &string, tagSearch) tmpSet << string.toLower();
		tagSearch.clear();
		foreach (const QString &string, tmpSet) tagSearch << string;
		statement.addWhere(QString("taggedEntries.id in (select id from taggedEntries where type = %1 and tagId in (select docid from tags where tag match '%2') group by id having count(id) == %3)").arg(entryType()).arg(tagSearch.join(" OR ")).arg(tagSearch.size()));
//		statement.setGroupBy(QueryBuilder::GroupBy("taggedEntries.id", QString("count(taggedEntries.id) = %1").arg(tagSearch.size())));
	}
}

QueryBuilder::Column EntrySearcher::canSort(const QString &sort, const QueryBuilder::Statement &statement)
{
	if (sort == "study") {
		return QueryBuilder::Column("training", "dateAdded is null");
	}
	else if (sort == "score") {
		return QueryBuilder::Column("training", "score");
	}
	return QueryBuilder::Column("0");
}

bool EntrySearcher::searchToCommands(const QStringList &searches, QList<SearchCommand> &commands) const
{
	foreach (const QString &search, searches) {
		if (commandMatch.exactMatch(search)) {
			SearchCommand command = SearchCommand::fromString(search);
			if (validCommands.contains(command.command())) commands << command;
			else return false;
		}
		else {
			SearchCommand command = commandFromWord(search);
			if (command.isValid()) commands << command;
			else return false;
		}
	}
	return true;
}

static QDateTime variantToDate(const QVariant &variant)
{
	if (variant.isNull()) return QDateTime();
	else return QDateTime::fromTime_t(variant.toUInt());
}

void EntrySearcher::loadMiscData(Entry *entry)
{
	QSqlQuery query;

	// Load training data
	query.prepare("select dateAdded, dateLastTrain, nbTrained, nbSuccess, dateLastMistake, score from training where type = ? and id = ?");
	query.addBindValue(entry->type());
	query.addBindValue(entry->id());
	query.exec();
	// The entry is registered, so lets load its data
	if (query.next()) {
		entry->setDateAdded(variantToDate(query.value(0)));
		entry->setDateLastTrained(variantToDate(query.value(1)));
		entry->setNbTrained(query.value(2).toInt());
		entry->setNbSuccess(query.value(3).toInt());
		entry->setDateLastMistake(variantToDate(query.value(4)));
		entry->_score = query.value(5).toInt();
	}

	// Tags data
	query.prepare("select tagId from taggedEntries where type = ? and id = ? order by date");
	query.addBindValue(entry->type());
	query.addBindValue(entry->id());
	query.exec();
	while (query.next())
		entry->_tags << Tag::getTag(query.value(0).toUInt());

	// Notes data
	query.prepare("select noteId, dateAdded, dateLastChange, note from notes join notesText on notes.noteId == notesText.docid where type = ? and id = ? order by dateAdded ASC, noteId ASC");
	query.addBindValue(entry->type());
	query.addBindValue(entry->id());
	query.exec();
	while (query.next()) {
		entry->_notes << Entry::Note(query.value(0).toInt(), QDateTime::fromTime_t(query.value(1).toInt()), QDateTime::fromTime_t(query.value(2).toInt()), query.value(3).toString());
	}
	
	// Lists data
	query.prepare("select lists.rowid, listsLabels.label from lists join listsLabels on lists.parent = listsLabels.rowid where lists.type = ? and lists.id = ?");
	query.addBindValue(entry->type());
	query.addBindValue(entry->id());
	query.exec();
	while (query.next()) {
		entry->_lists << query.value(0).toUInt();
	}
}

void EntrySearcher::setColumns(QueryBuilder::Statement &statement) const
{
	statement.setDistinct(true);
	statement.addJoin(QueryBuilder::Join(QueryBuilder::Column("training", "id"), QString("training.type = %1").arg(entryType()), QueryBuilder::Join::Left));
	statement.autoJoin();
	// Add the entry type and id columns
	statement.addColumn(QueryBuilder::Column(QString::number(entryType())), 0);
	QueryBuilder::Column leftColumn = statement.leftColumn();
	statement.addColumn(leftColumn, 1);
	statement.setGroupBy(leftColumn.toString());
}

