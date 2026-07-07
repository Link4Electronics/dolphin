// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <variant>

#include "Common/BitUtils.h"
#include "Common/Swap.h"

#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"

namespace WiimoteEmu
{
struct DesiredExtensionState
{
private:
  template <ExtensionNumber N, typename T>
  struct ExtNumTypePair
  {
    static constexpr ExtensionNumber ext_num = N;
    using ext_type = T;
  };

  template <typename... Ts>
  struct ExtDataImpl
  {
    using type = std::variant<std::monostate, typename Ts::ext_type::DesiredState...>;

    static_assert((std::is_same_v<std::variant_alternative_t<Ts::ext_num, type>,
                                  typename Ts::ext_type::DesiredState> &&
                   ...),
                  "Please use ExtensionNumber enum order for DTM file index consistency.");
  };

public:
  using ExtensionData =
      ExtDataImpl<ExtNumTypePair<ExtensionNumber::NUNCHUK, Nunchuk>,
                  ExtNumTypePair<ExtensionNumber::CLASSIC, Classic>,
                  ExtNumTypePair<ExtensionNumber::GUITAR, Guitar>,
                  ExtNumTypePair<ExtensionNumber::DRUMS, Drums>,
                  ExtNumTypePair<ExtensionNumber::TURNTABLE, Turntable>,
                  ExtNumTypePair<ExtensionNumber::UDRAW_TABLET, UDrawTablet>,
                  ExtNumTypePair<ExtensionNumber::DRAWSOME_TABLET, DrawsomeTablet>,
                  ExtNumTypePair<ExtensionNumber::TATACON, TaTaCon>,
                  ExtNumTypePair<ExtensionNumber::SHINKANSEN, Shinkansen>>::type;

  ExtensionData data = std::monostate{};
};

template <typename T>
void DefaultExtensionUpdate(EncryptedExtension::Register* reg,
                            const DesiredExtensionState& target_state)
{
  if (std::holds_alternative<T>(target_state.data))
  {
    Common::BitCastPtr<T>(&reg->controller_data) = std::get<T>(target_state.data);
  }
  else
  {
    Common::BitCastPtr<T>(&reg->controller_data) = T{};
  }

  // Extension protocols use little-endian byte order for multi-byte fields.
  // On big-endian hosts, swap the trailing u16 button field so the register
  // bytes match what the PPC (which reads them as LE bytes) expects.
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  if constexpr (std::is_same_v<T, Classic::DataFormat> ||
                std::is_same_v<T, Guitar::DataFormat> ||
                std::is_same_v<T, Turntable::DataFormat>)
  {
    constexpr size_t offset = sizeof(T) - sizeof(u16);
    u16* last_u16 = reinterpret_cast<u16*>(&reg->controller_data[offset]);
    *last_u16 = Common::swap16(*last_u16);
  }
#endif
}
}  // namespace WiimoteEmu
