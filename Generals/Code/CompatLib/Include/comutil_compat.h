/*
**	Command & Conquer Generals(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// GeneralsX @build fbraz 10/02/2026 Bender
// COM Utility header compatibility stub
// Used by WW3D2/dx8webbrowser.cpp (DirectX web browser embedding)
// Phase 3 out-of-scope: Minimal stubs to allow compilation

#ifndef _WIN32

// _com_error class stub (COM error handling)
class _com_error {
public:
    explicit _com_error(long hr) : m_hr(hr) {}
    long Error() const { return m_hr; }
    const wchar_t* ErrorMessage() const { return L"COM Error"; }
    
private:
    long m_hr;
};

// _bstr_t class stub (BSTR wrapper)
class _bstr_t {
public:
    _bstr_t() : m_str(nullptr) {}
    explicit _bstr_t(const char*) : m_str(nullptr) {}
    explicit _bstr_t(const wchar_t*) : m_str(nullptr) {}
    ~_bstr_t() {}
    
    operator const char*() const { return ""; }
    operator const wchar_t*() const { return L""; }
    
private:
    void* m_str;
};

// _variant_t class stub (VARIANT wrapper)
class _variant_t {
public:
    _variant_t() {}
    explicit _variant_t(short) {}
    explicit _variant_t(long) {}
    explicit _variant_t(float) {}
    explicit _variant_t(double) {}
    explicit _variant_t(const char*) {}
    explicit _variant_t(const wchar_t*) {}
    explicit _variant_t(bool) {}
    ~_variant_t() {}
};

#endif // !_WIN32
