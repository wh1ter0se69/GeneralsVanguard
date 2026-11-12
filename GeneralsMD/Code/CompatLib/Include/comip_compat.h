/*
**	Command & Conquer Generals Zero Hour(tm)
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

// GeneralsX @build fbraz 11/02/2026 Bender
// COM Interface Pointer compatibility stub
// Used by WW3D2/dx8webbrowser.cpp (DirectX web browser embedding)
// Phase 3 out-of-scope: Minimal stubs to allow compilation

#ifndef _WIN32

#include "com_compat.h"

// _com_ptr_t template class stub (smart pointer for COM interfaces)
template<typename Interface>
class _com_ptr_t {
public:
    _com_ptr_t() : m_ptr(nullptr) {}
    explicit _com_ptr_t(Interface* p) : m_ptr(p) {}
    ~_com_ptr_t() { Release(); }
    
    // Assignment
    _com_ptr_t& operator=(Interface* p) {
        if (m_ptr != p) {
            Release();
            m_ptr = p;
        }
        return *this;
    }
    
    // Dereference
    Interface* operator->() const { return m_ptr; }
    Interface& operator*() const { return *m_ptr; }
    
    // Conversion
    operator Interface*() const { return m_ptr; }
    Interface** operator&() { return &m_ptr; }
    
    // Access
    Interface* GetInterfacePtr() const { return m_ptr; }
    
    // Release
    void Release() {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }
    
    // Query interface
    template<typename OtherInterface>
    HRESULT QueryInterface(OtherInterface** pp) const {
        if (!m_ptr) return 0x80004003; // E_POINTER
        return m_ptr->QueryInterface(__uuidof(OtherInterface), (void**)pp);
    }
    
private:
    Interface* m_ptr;
};

// __uuidof stub (returns dummy GUID)
template<typename T>
inline const IID& __uuidof(const T&) {
    static const IID dummy = { 0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0} };
    return dummy;
}

#endif // !_WIN32
