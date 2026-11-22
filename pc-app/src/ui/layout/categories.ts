import { Activity, Hand, Palette, QrCode, Route, ScanFace, ScanQrCode, ScanSearch, Shapes, type LucideIcon } from 'lucide-react'
import type { FunctionId } from './types'

/** Each rail item has a category colour (--cat-* in theme.css), as categories do in a block editor. */
export type Category = 'color' | 'tag' | 'face' | 'gesture' | 'line' | 'track' | 'qr' | 'shape' | 'motion' | 'settings' | 'firmware'

export const FN_CATEGORY: Record<FunctionId, Category> = {
  1: 'color', 2: 'tag', 3: 'face', 4: 'line', 5: 'track', 6: 'qr', 8: 'gesture', 11: 'shape', 12: 'motion',
}
export const FN_ICON: Record<FunctionId, LucideIcon> = {
  1: Palette, 2: QrCode, 3: ScanFace, 4: Route, 5: ScanSearch, 6: ScanQrCode, 8: Hand, 11: Shapes, 12: Activity,
}
