/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_NodeLoader_h
#define mozilla_dom_NodeLoader_h

#include "nsINodeLoader.h"
#include "nsISupports.h"
#include "NodeBindings.h"
#include "nsITimer.h"

class NodeLoader final : public nsINodeLoader
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSINODELOADER

  NodeLoader();
  RefPtr<mozilla::NodeBindings> nodeBindings;
  nsCOMPtr<nsITimer> timer;

private:
  ~NodeLoader();

protected:
};

#endif // mozilla_dom_NodeLoader_h