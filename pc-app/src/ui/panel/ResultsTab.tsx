import { useSyncExternalStore } from "react";
import type { ResultsStore } from "../../device/resultsStore";
import type { LearnedId, Result } from "../../device/types";
import { useT, type MessageKey } from "../../i18n";
import type { FunctionId } from "../layout/types";
import { FN_KIND, idColorVar, shapeName, signed } from "../live/labels";
import { useMirror } from "../live/mirror";

interface Props {
  fn: FunctionId;
  /** The newest results: what the R4 would read. */
  results: ResultsStore;
  learned: LearnedId[];
}

function LineTable({ rows, mirror }: { rows: Result[]; mirror: boolean }) {
  const { t } = useT();
  return (
    <>
      <table className="rp-table">
        <thead>
          <tr>
            <th scope="col">{t("colAngle")}</th>
            <th scope="col">{t("colOffset")}</th>
            <th scope="col">{t("colStart")}</th>
            <th scope="col">{t("colEnd")}</th>
            <th scope="col">{t("colFlags")}</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r, i) => {
            const l = r.line;
            const flags = [l?.cross ? t("lineCross") : "", l?.branch ? t("lineBranch") : ""].filter(Boolean).join(" ");
            return (
              <tr key={i}>
                <td>{signed(l?.angleDeg ?? 0)}°</td>
                <td>{signed(l?.offsetX ?? 0)}</td>
                <td>{l ? `${l.start[0]}, ${l.start[1]}` : "—"}</td>
                <td>{l ? `${l.end[0]}, ${l.end[1]}` : "—"}</td>
                <td>{flags || "—"}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {mirror && <p className="muted">{t("coordsTrueImage")}</p>}
    </>
  );
}

export default function ResultsTab({ fn, results: store, learned }: Props) {
  const { t } = useT();
  const [mirror] = useMirror();
  const results = useSyncExternalStore(store.subscribe, store.latest);
  const rows = results.filter((r) => r.kind === FN_KIND[fn]);
  if (rows.length === 0) {
    // Object tracking: nothing to show means either no object yet or the target is gone.
    if (fn === 5) return <p className="muted">{t(learned.length > 0 ? "targetLost" : "notLearnedYet")}</p>;
    return <p className="muted">{t("resultsEmpty")}</p>;
  }
  if (fn === 4) return <LineTable rows={rows} mirror={mirror} />;
  const face = fn === 3;
  const gesture = fn === 8;
  const track = fn === 5;
  const qr = fn === 6;
  const shape = fn === 11;
  const motion = fn === 12;
  // Motion has nothing to name; every other function has a name column (empty when unnamed).
  const named = !motion;
  const scored = face || gesture || track || shape || motion;
  const idText = (r: Result) =>
    r.kind === "face"
      ? t("catFace")
      : r.kind === "motion"
        ? t("motionLabel")
        : r.kind === "gesture" || r.kind === "shape"
          ? String(r.id)
          : r.kind === "tag"
            ? `Tag ${r.id}`
            : r.kind === "qr"
              ? r.id
                ? `ID ${r.id}`
                : t("qrUnlearned")
              : r.id
                ? `ID${r.id}`
                : "—";
  return (
    <>
      <table className="rp-table">
        <thead>
          <tr>
            <th scope="col">{t("colId")}</th>
            {gesture && <th scope="col">{t("colGesture")}</th>}
            {shape && <th scope="col">{t("colShape")}</th>}
            {named && <th scope="col">{t("colName")}</th>}
            {qr && <th scope="col" className="rp-content">{t("colContent")}</th>}
            <th scope="col">x</th>
            <th scope="col">y</th>
            <th scope="col">{t("colWidth")}</th>
            <th scope="col">{t("colHeight")}</th>
            {scored && <th scope="col">{t("colConfidence")}</th>}
          </tr>
        </thead>
        <tbody>
          {rows.map((r, i) => (
            <tr key={i}>
              <td>
                <span
                  className="rp-dot"
                  style={{ background: idColorVar(r) }}
                  aria-hidden="true"
                />
                {idText(r)}
              </td>
              {gesture && <td>{t(`gesture${r.id}` as MessageKey)}</td>}
              {shape && <td className="rp-name">{shapeName(r.id, t)}</td>}
              {named && (
                <td className="rp-name">
                  {learned.find((l) => l.id === r.id)?.name ?? ""}
                </td>
              )}
              {qr && (
                <td className="rp-content">
                  {r.text ?? "—"}
                  {r.truncated && <small>{t("qrTruncated")}</small>}
                </td>
              )}
              <td>{Math.round(r.x)}</td>
              <td>{Math.round(r.y)}</td>
              <td>{Math.round(r.width)}</td>
              <td>{Math.round(r.height)}</td>
              {scored && <td>{Math.round(r.confidence ?? 0)}%</td>}
            </tr>
          ))}
        </tbody>
      </table>
      {mirror && <p className="muted">{t("coordsTrueImage")}</p>}
    </>
  );
}
