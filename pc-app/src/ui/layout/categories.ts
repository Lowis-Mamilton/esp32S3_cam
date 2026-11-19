import { Hand, Palette, QrCode, ScanFace, type LucideIcon } from 'lucide-react'
import type { FunctionId } from './types'

/** Each rail item has a category colour (--cat-* in theme.css), as categories do in a block editor. */
export type Category = 'color' | 'tag' | 'face' | 'gesture' | 'settings' | 'firmware'

export const FN_CATEGORY: Record<FunctionId, Category> = { 1: 'color', 2: 'tag', 3: 'face', 4: 'color', 5: 'color', 8: 'gesture' } // TODO M6 Task 7: line / track categories
export const FN_ICON: Record<FunctionId, LucideIcon> = { 1: Palette, 2: QrCode, 3: ScanFace, 4: Palette, 5: Palette, 8: Hand } // TODO M6 Task 7: line / track icons
