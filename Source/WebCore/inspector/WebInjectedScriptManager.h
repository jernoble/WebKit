/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CommandLineAPIHost.h"
#include <JavaScriptCore/InjectedScriptManager.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class LocalDOMWindow;

class WebInjectedScriptManager final : public Inspector::InjectedScriptManager {
    WTF_MAKE_NONCOPYABLE(WebInjectedScriptManager);
    WTF_MAKE_TZONE_ALLOCATED(WebInjectedScriptManager);
public:
    WebInjectedScriptManager(Inspector::InspectorEnvironment&, Ref<Inspector::InjectedScriptHost>&&);
    ~WebInjectedScriptManager() override = default;

    const RefPtr<CommandLineAPIHost>& commandLineAPIHost() const { return m_commandLineAPIHost; }

    void connect() override;
    void disconnect() override;
    void discardInjectedScripts() override;

    void discardInjectedScriptsFor(LocalDOMWindow&);

private:
    void didCreateInjectedScript(const Inspector::InjectedScript&) override;

    RefPtr<CommandLineAPIHost> m_commandLineAPIHost;
};

} // namespace WebCore
