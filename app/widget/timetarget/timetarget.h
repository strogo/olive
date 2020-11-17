/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2020 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef TIMETARGETOBJECT_H
#define TIMETARGETOBJECT_H

#include "node/node.h"

OLIVE_NAMESPACE_ENTER

class TimeTargetObject
{
public:
  TimeTargetObject();

  Node* GetTimeTarget() const;
  void SetTimeTarget(Node* target);

  void SetPathIndex(int index);

  rational GetAdjustedTime(Node* from, Node* to, const rational& r, NodeParam::Type direction) const;
  TimeRange GetAdjustedTime(Node* from, Node* to, const TimeRange& r, NodeParam::Type direction) const;

  //int GetNumberOfPathAdjustments(Node* from, NodeParam::Type direction) const;

protected:
  virtual void TimeTargetChangedEvent(Node* ){}

private:
  Node* time_target_;

  int path_index_;

};

OLIVE_NAMESPACE_EXIT

#endif // TIMETARGETOBJECT_H
