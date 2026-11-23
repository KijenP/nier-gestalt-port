// nier - the PC port's own texts (see pc_text.h).

#include "pc_text.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "achievements.h"
#include "game_text.h"

namespace nier::pc_text {
namespace {

namespace fs = std::filesystem;

struct Entry {
  uint32_t id;
  const char* en;
  const char* fr;
  const char* de;
};

// The game formats these texts printf-style and has no escape for %: no text may hold a %.
// Ids 9000000 and up are the port's (the game's own go to 6999999); achievements are
// 9500000 + id * 10 + field. Never renumber a row: translations refer to them by id.
const Entry kEntries[] = {
    // Rows the port adds to the game's Options menus.
    {9000100, "Video Options", "Options vidéo", "Videooptionen"},
    {9000110, "Adjust resolution, image quality and display settings.",
     "Régler la résolution, la qualité d'image et l'affichage.", "Auflösung, Bildqualität und Anzeige einstellen."},
    {9000120, "Control Options", "Commandes", "Steuerung"},
    {9000130, "Touch, keyboard and mouse, and controller.", "Tactile, clavier et souris, et manette.", "Touch, Tastatur und Maus, und Controller."},
    {9000140, "Language", "Langue", "Sprache"},
    {9000150, "Change the language of the game text.", "Changer la langue du texte du jeu.",
     "Sprache des Spieltexts ändern."},
    {9000160, "Quit to Desktop", "Quitter le jeu", "Spiel beenden"},
    {9000170, "Close the game. Progress since the last save is lost.",
     "Fermer le jeu. La progression non sauvegardée est perdue.",
     "Spiel schließen. Nicht gespeicherter Fortschritt geht verloren."},
    {9000180, "Quit the game and return to the desktop?", "Quitter le jeu et revenir au bureau ?",
     "Spiel beenden und zum Desktop zurückkehren?"},

    // Video Options.
    {9001000, "Performance Meter", "Compteur de performances", "Leistungsanzeige"},
    {9001010, "Shows real fps, frame time, GPU and CPU use. Shortcut: F2.",
     "Affiche les fps réels, le temps d'image et l'usage GPU et CPU. Raccourci : F2.",
     "Zeigt echte fps, Bildzeit sowie GPU- und CPU-Last. Kürzel: F2."},
    {9001020, "Window Mode", "Mode d'affichage", "Fenstermodus"},
    {9001030, "Fullscreen or windowed.", "Plein écran ou fenêtré.", "Vollbild oder Fenster."},
    {9001040, "Fullscreen", "Plein écran", "Vollbild"},
    {9001050, "Windowed", "Fenêtré", "Fenster"},
    {9001060, "Resolution", "Résolution", "Auflösung"},
    {9001070, "Window size and how many pixels are drawn.", "Taille de la fenêtre et nombre de pixels dessinés.",
     "Fenstergröße und wie viele Pixel gezeichnet werden."},
    {9001080, "Resolution Scale", "Échelle de résolution", "Auflösungsskalierung"},
    {9001090, "Below full size draws fewer pixels and scales up (use AMD FSR): much lighter. Above: sharper but heavier.",
     "Sous la taille réelle, moins de pixels sont dessinés puis agrandis (utilisez AMD FSR) : bien plus léger. Au-dessus : plus "
     "net mais plus lourd.",
     "Unter voller Größe werden weniger Pixel gezeichnet und hochskaliert (AMD FSR nutzen): viel leichter. Darüber: schärfer, "
     "aber schwerer."},
    {9001100, "3D Sharpness", "Netteté 3D", "3D-Schärfe"},
    {9001110,
     "The game's inner 3D resolution. Auto stays Highest while the frame rate limit is reached and lowers it only as "
     "much as needed.",
     "Résolution 3D interne du jeu. Auto reste au maximum tant que la limite d'images est atteinte et ne la baisse "
     "que si nécessaire.",
     "Interne 3D-Auflösung des Spiels. Auto bleibt auf Höchste, solange das Bildratenlimit erreicht wird, und senkt "
     "sie nur so weit wie nötig."},
    {9001120, "Auto", "Auto", "Auto"},
    {9001130, "Lowest", "Minimale", "Niedrigste"},
    {9001140, "Medium", "Moyenne", "Mittel"},
    {9001150, "Highest", "Maximale", "Höchste"},
    {9001160, "Lowest 3D Sharpness", "Netteté 3D minimale", "Niedrigste 3D-Schärfe"},
    {9001170, "Size of the Lowest level (and of Auto when the game struggles). Smaller is lighter.",
     "Taille du niveau minimal (et d'Auto quand le jeu peine). Plus petit est plus léger.",
     "Größe der niedrigsten Stufe (und von Auto, wenn das Spiel kämpft). Kleiner ist leichter."},
    {9001180, "768x432 (original)", "768x432 (original)", "768x432 (Original)"},
    {9001190, "Frame Rate", "Fréquence d'images", "Bildrate"},
    {9001200, "60 fps in gameplay. Heavier: at 1080p or more the GPU may not keep up.",
     "60 fps en jeu. Plus lourd : en 1080p ou plus, le GPU peut ne pas suivre.",
     "60 fps im Spiel. Schwerer: ab 1080p kommt die GPU eventuell nicht mit."},
    {9001210, "V-Sync", "Synchro verticale", "V-Sync"},
    {9001220, "Off, the game loses its frame pacing. Recommended: on.",
     "Désactivée, le jeu perd la régularité de ses images. Recommandé : activée.",
     "Aus verliert das Spiel seinen Bildtakt. Empfohlen: an."},
    {9001230, "Off (unstable)", "Désactivée (instable)", "Aus (instabil)"},
    {9001240, "Off, frames do not wait for the display (may tear) and can pass its rate.",
     "Désactivée, les images n'attendent pas l'écran (déchirures possibles) et peuvent dépasser sa fréquence.",
     "Aus warten die Bilder nicht auf den Bildschirm (Tearing möglich) und können seine Rate übersteigen."},
    {9001250, "Frame Rate Limit", "Limite d'images", "Bildratenlimit"},
    {9001260, "Frames per second, up to Unlimited. With V-Sync on the display also limits it. More fps is heavier.",
     "Images par seconde, jusqu'à Illimité. Avec la synchro verticale, l'écran la limite aussi. Plus de fps est plus "
     "lourd.",
     "Bilder pro Sekunde, bis Unbegrenzt. Mit V-Sync begrenzt auch der Bildschirm. Mehr fps ist schwerer."},
    {9001270, "Unlimited", "Illimité", "Unbegrenzt"},
    {9001280, "Anti-Aliasing (MSAA)", "Anticrénelage (MSAA)", "Kantenglättung (MSAA)"},
    {9001290, "Smooths jagged edges. Each step costs more GPU time.",
     "Adoucit les bords crénelés. Chaque niveau coûte plus de temps GPU.",
     "Glättet gezackte Kanten. Jede Stufe kostet mehr GPU-Zeit."},
    {9001300, "Anisotropic Filter", "Filtre anisotrope", "Anisotroper Filter"},
    {9001310, "Sharp textures at slanted angles (floors, walls).",
     "Textures nettes vues de biais (sols, murs).", "Scharfe Texturen in schrägen Winkeln (Böden, Wände)."},
    {9001320, "Game's", "Du jeu", "Des Spiels"},
    {9001330, "Anti-Aliasing", "Anticrénelage", "Kantenglättung"},
    {9001340, "Smooths jagged edges.", "Adoucit les bords crénelés.", "Glättet gezackte Kanten."},
    {9001350, "FXAA Extreme", "FXAA extrême", "FXAA Extrem"},
    {9001360, "MSAA 2x", "MSAA 2x", "MSAA 2x"},
    {9001370, "The Xbox 360's own anti-aliasing.", "L'anticrénelage de la Xbox 360.",
     "Die eigene Kantenglättung der Xbox 360."},
    {9001380, "Upscaling", "Mise à l'échelle", "Hochskalierung"},
    {9001390, "How the image is scaled to the screen.", "Comment l'image est adaptée à l'écran.",
     "Wie das Bild auf den Bildschirm skaliert wird."},
    {9001400, "Bilinear", "Bilinéaire", "Bilinear"},
    {9001410, "AMD CAS (sharpness)", "AMD CAS (netteté)", "AMD CAS (Schärfe)"},
    {9001420, "AMD FSR", "AMD FSR", "AMD FSR"},
    {9001430, "Black Bars", "Bandes noires", "Schwarze Balken"},
    {9001440, "Keeps 16:9 on screens of another shape.", "Garde le 16:9 sur les écrans d'un autre format.",
     "Behält 16:9 auf Bildschirmen anderer Form."},
    {9001450, "Full Effects", "Effets complets", "Volle Effekte"},
    {9001460, "Fog and other full-screen effects. Recommended: on.",
     "Brouillard et autres effets plein écran. Recommandé : activés.",
     "Nebel und andere Vollbildeffekte. Empfohlen: an."},
    {9001470, "Off", "Désactivé", "Aus"},
    {9001480, "On", "Activé", "An"},
    {9001490, " (Takes effect after restarting the game.)", " (Prend effet après le redémarrage du jeu.)",
     " (Wirkt nach einem Neustart des Spiels.)"},
    {9001500, "Brightness", "Luminosité", "Helligkeit"},
    {9001510, "Lightens or darkens the picture's mid tones (black and white stay).",
     "Éclaircit ou assombrit les tons moyens de l'image (le noir et le blanc restent).",
     "Hellt die Mitteltöne des Bildes auf oder dunkelt sie ab (Schwarz und Weiß bleiben)."},

    // Control Options.
    {9002000, "Mouse Camera", "Caméra à la souris", "Mauskamera"},
    {9002010, "The mouse turns the camera (the cursor is hidden in game). Off: camera on the arrows.",
     "La souris tourne la caméra (le curseur est caché en jeu). Désactivée : caméra sur les flèches.",
     "Die Maus dreht die Kamera (der Zeiger ist im Spiel versteckt). Aus: Kamera auf den Pfeiltasten."},
    {9002020, "Off (arrows)", "Désactivée (flèches)", "Aus (Pfeiltasten)"},
    {9002030, "Mouse Sensitivity", "Sensibilité de la souris", "Mausempfindlichkeit"},
    {9002040, "Camera speed with the mouse.", "Vitesse de la caméra à la souris.", "Kamerageschwindigkeit mit der Maus."},
    {9002050, "Move: Forward", "Avancer", "Vorwärts"},
    {9002060, "Move: Back", "Reculer", "Rückwärts"},
    {9002070, "Move: Left", "Aller à gauche", "Nach links"},
    {9002080, "Move: Right", "Aller à droite", "Nach rechts"},
    {9002090, "Jump", "Sauter", "Springen"},
    {9002100, "Action", "Action", "Aktion"},
    {9002110, "Attack", "Attaque", "Angriff"},
    {9002120, "Heavy Attack", "Attaque puissante", "Starker Angriff"},
    {9002130, "Recenter Camera", "Recentrer la caméra", "Kamera zentrieren"},
    {9002140, "Dodge", "Esquive", "Ausweichen"},
    {9002150, "Magic 1", "Magie 1", "Magie 1"},
    {9002160, "Guard", "Garde", "Blocken"},
    {9002170, "Magic 2", "Magie 2", "Magie 2"},
    {9002180, "Camera: Up", "Caméra : haut", "Kamera: oben"},
    {9002190, "Camera: Down", "Caméra : bas", "Kamera: unten"},
    {9002200, "Camera: Left", "Caméra : gauche", "Kamera: links"},
    {9002210, "Camera: Right", "Caméra : droite", "Kamera: rechts"},
    {9002220, "Walk", "Marcher", "Gehen"},
    {9002230, "Main Menu", "Menu principal", "Hauptmenü"},
    {9002240, "Large Map", "Grande carte", "Große Karte"},
    {9002250, "Item Shortcut", "Raccourci d'objets", "Gegenstandskürzel"},
    {9002260, "One-Handed Sword", "Épée à une main", "Einhandschwert"},
    {9002270, "Two-Handed Sword", "Épée à deux mains", "Zweihandschwert"},
    {9002280, "Spear", "Lance", "Speer"},
    {9002290, "Press a key...", "Appuyez sur une touche...", "Taste drücken..."},
    {9002300, "Press A, then the new key. Esc cancels.", "Appuyez sur A, puis sur la nouvelle touche. Échap annule.",
     "A drücken, dann die neue Taste. Esc bricht ab."},
    {9002310, "Restore Defaults", "Valeurs par défaut", "Standard wiederherstellen"},
    {9002320, "Puts every key back to the default.", "Remet toutes les touches par défaut.",
     "Setzt alle Tasten auf den Standard zurück."},
    {9002330, "Left Click", "Clic gauche", "Linksklick"},
    {9002340, "Right Click", "Clic droit", "Rechtsklick"},
    {9002350, "Middle Click", "Clic central", "Mittelklick"},
    {9002360, "Enter", "Entrée", "Eingabe"},
    {9002370, "Esc", "Échap", "Esc"},
    {9002380, "Ctrl", "Ctrl", "Strg"},
    {9002390, "Page Up", "Page préc.", "Bild auf"},
    {9002400, "Page Down", "Page suiv.", "Bild ab"},

    // Control Options on Android: the on-screen buttons (touch_controls.h).
    {9005000, "On-Screen Buttons", "Boutons à l'écran", "Bildschirmtasten"},
    {9005010, "Automatic", "Automatique", "Automatisch"},
    {9005020, "Always", "Toujours", "Immer"},
    {9005030, "Off", "Désactivé", "Aus"},
    {9005040, "Automatic: shown while no controller is connected.",
     "Automatique : affichés tant qu'aucune manette n'est connectée.",
     "Automatisch: sichtbar, solange kein Controller verbunden ist."},
    {9005050, "Button Size", "Taille des boutons", "Tastengröße"},
    {9005060, "Size of all the on-screen buttons.", "Taille de tous les boutons à l'écran.",
     "Größe aller Bildschirmtasten."},
    {9005070, "Button Opacity", "Opacité des boutons", "Tastendeckkraft"},
    {9005080, "How visible the on-screen buttons are.", "Visibilité des boutons à l'écran.",
     "Wie sichtbar die Bildschirmtasten sind."},
    {9005090, "Touch Camera Speed", "Vitesse de la caméra tactile", "Touch-Kamerageschwindigkeit"},
    {9005100, "How fast the camera turns when you drag on the right side of the screen.",
     "Vitesse de la caméra quand vous glissez sur la droite de l'écran.",
     "Wie schnell sich die Kamera dreht, wenn Sie rechts auf dem Bildschirm wischen."},
    {9005110, "Edit Button Layout", "Disposer les boutons", "Tasten anordnen"},
    {9005120, "Drag the buttons to new places and change their size.",
     "Faites glisser les boutons et changez leur taille.",
     "Tasten verschieben und ihre Größe ändern."},
    {9005130, "Puts the on-screen buttons back as they were.", "Remet les boutons à l'écran comme au départ.",
     "Setzt die Bildschirmtasten zurück."},
    {9005140, "Default", "Par défaut", "Standard"},
    {9005150, "Cancel", "Annuler", "Abbrechen"},
    {9005160, "Save", "Enregistrer", "Speichern"},
    {9005170, "Drag the buttons. Tap one and use - / + to change its size.",
     "Faites glisser les boutons. Touchez-en un et utilisez - / + pour sa taille.",
     "Tasten ziehen. Eine antippen und mit - / + die Größe ändern."},

    // Control Options' three pages (2026-11-17).
    {9006000, "Touch Controls", "Commandes tactiles", "Touch-Steuerung"},
    {9006010, "The on-screen buttons: when they show, their size and places.", "Les boutons à l'écran : quand ils s'affichent, leur taille et leur place.", "Die Bildschirmtasten: wann sie erscheinen, Größe und Position."},
    {9006020, "Mouse and Keyboard", "Souris et clavier", "Maus und Tastatur"},
    {9006030, "The keys of each action and the mouse camera.", "Les touches de chaque action et la caméra à la souris.", "Die Tasten jeder Aktion und die Mauskamera."},
    {9006040, "Controller", "Manette", "Controller"},
    {9006050, "Which controller button does each action.", "Quel bouton de la manette fait chaque action.", "Welche Controllertaste welche Aktion ausführt."},
    {9006060, "Back to Control Options", "Retour aux commandes", "Zurück zur Steuerung"},
    {9006070, "Back to the list of controls.", "Retour à la liste des commandes.", "Zurück zur Liste der Steuerungen."},
    {9006080, "Press a button...", "Appuyez sur un bouton...", "Taste drücken..."},
    {9006090, "Press A, then the new button. Two actions trade buttons.", "Appuyez sur A, puis sur le nouveau bouton. Deux actions échangent leurs boutons.", "A drücken, dann die neue Taste. Zwei Aktionen tauschen Tasten."},
    {9006100, "Every button back to its own action.", "Chaque bouton retrouve sa propre action.", "Jede Taste wieder mit ihrer eigenen Aktion."},
    {9006110, "Left Stick Press", "Clic du stick gauche", "Linken Stick drücken"},

    // Video Options added later (2026-11-17: they showed in English).
    {9006200, "Below 100% draws fewer pixels and scales up (use AMD FSR): much lighter. Above: sharper but heavier.", "Sous 100 pour cent, moins de pixels sont dessinés puis agrandis (utilisez AMD FSR) : bien plus léger. Au-dessus : plus net mais plus lourd.", "Unter 100 Prozent werden weniger Pixel gezeichnet und hochskaliert (AMD FSR nutzen): viel leichter. Darüber: schärfer, aber schwerer."},
    {9006210, "How the image is scaled to the screen. AMD FSR upscales with sharp edges, but only gains speed with Resolution Scale below 100%: 75% = Quality, 67% = Balanced, 50% = Performance.", "Comment l'image est mise à l'échelle. AMD FSR agrandit avec des bords nets, mais n'accélère qu'avec l'échelle de résolution sous 100 pour cent : 75 = Qualité, 67 = Équilibré, 50 = Performance.", "Wie das Bild auf den Bildschirm skaliert wird. AMD FSR skaliert mit scharfen Kanten, bringt aber nur mit einer Auflösungsskala unter 100 Prozent Tempo: 75 = Qualität, 67 = Ausgewogen, 50 = Leistung."},
    {9006220, "Button Icons", "Icônes des boutons", "Tastensymbole"},
    {9006230, "The controller's icons in hints and menus. With the keyboard and mouse the keys show.", "Les icônes de la manette dans les aides et les menus. Avec le clavier et la souris, les touches s'affichent.", "Die Controllersymbole in Hinweisen und Menüs. Mit Tastatur und Maus erscheinen die Tasten."},
    {9006240, "Screen Fit", "Ajustement de l'écran", "Bildanpassung"},
    {9006250, "Fill (crops)", "Remplir (rogne)", "Füllen (beschneidet)"},
    {9006260, "Real Widescreen", "Vrai écran large", "Echtes Breitbild"},
    {9006270, "The game is 16:9. On wider screens: Fill zooms in, cropping a little off the top and bottom; Black Bars shows the whole picture; Stretch fills the screen, widening the picture; Widescreen fills the screen with the HUD in shape in the middle; Real Widescreen opens the camera to the sides (more view, nothing stretched) and takes the HUD to the edges.", "Le jeu est en 16:9. Sur les écrans plus larges : Remplir zoome en rognant un peu en haut et en bas ; Bandes noires montre toute l'image ; Étirer remplit l'écran en élargissant l'image ; Écran large remplit l'écran avec l'interface intacte au milieu ; Vrai écran large ouvre la caméra sur les côtés (plus de vue, rien d'étiré) et place l'interface sur les bords.", "Das Spiel ist 16:9. Auf breiteren Bildschirmen: Füllen zoomt hinein und schneidet oben und unten etwas ab; Schwarze Balken zeigt das ganze Bild; Strecken füllt den Bildschirm und verbreitert das Bild; Breitbild füllt den Bildschirm mit unverzerrter Oberfläche in der Mitte; Echtes Breitbild öffnet die Kamera zu den Seiten (mehr Sicht, nichts gestreckt) und bringt die Oberfläche an die Ränder."},
    {9006280, "Near Blur", "Flou de près", "Nahunschärfe"},
    {9006290, "On (game's)", "Activé (du jeu)", "An (vom Spiel)"},
    {9006300, "The blur on characters when the camera comes close. Off: they stay sharp (better at 4K). The far blur stays.", "Le flou sur les personnages quand la caméra s'approche. Désactivé : ils restent nets (mieux en 4K). Le flou lointain reste.", "Die Unschärfe auf Figuren, wenn die Kamera nah kommt. Aus: Sie bleiben scharf (besser in 4K). Die Fernunschärfe bleibt."},

    // The quick weapon switch and its tutorial (2026-11-17).
    {9006400, "Weapon Switch", "Changement d'arme", "Waffenwechsel"},
    {9006410, "Tap", "Appuyer", "Tippen"},
    {9006420, "Hold", "Maintenir", "Halten"},
    {9006430, "Tap: a tap on a weapon button equips that kind's weapon at once, holding opens the menu. Hold: the other way round. Once Nier has every kind of weapon.", "Appuyer : un appui sur un bouton d'arme équipe aussitôt l'arme de ce type, le maintenir ouvre le menu. Maintenir : l'inverse. Dès que Nier a tous les types d'armes.", "Tippen: Ein Tippen auf eine Waffentaste rüstet sofort die Waffe dieser Art aus, Halten öffnet das Menü. Halten: umgekehrt. Sobald Nier jede Waffenart hat."},
    {9006440, "Quick Weapon Switch", "Changement rapide d'arme", "Schneller Waffenwechsel"},
    {9006450, "You now have every kind of weapon. Tap a weapon button (D-pad left, right or down; keys 1, 2 and 3; or the on-screen weapon button) to equip at once the weapon chosen for that kind. Hold it to open the weapon menu and choose another. To swap tap and hold: Options > Control Options > Weapon Switch.", "Vous avez maintenant tous les types d'armes. Appuyez sur un bouton d'arme (croix gauche, droite ou bas ; touches 1, 2 et 3 ; ou le bouton d'armes à l'écran) pour équiper aussitôt l'arme choisie de ce type. Maintenez-le pour ouvrir le menu des armes et en choisir une autre. Pour inverser : Options > Commandes > Changement d'arme.", "Du hast jetzt jede Waffenart. Tippe auf eine Waffentaste (Steuerkreuz links, rechts oder unten; Tasten 1, 2 und 3; oder die Waffentaste auf dem Bildschirm), um sofort die gewählte Waffe dieser Art auszurüsten. Halte sie, um das Waffenmenü zu öffnen und eine andere zu wählen. Zum Tauschen: Optionen > Steuerung > Waffenwechsel."},
    {9006460, "You now have every kind of weapon. Hold a weapon button (D-pad left, right or down; keys 1, 2 and 3; or the on-screen weapon button) to equip at once the weapon chosen for that kind. A tap opens the weapon menu to choose another. To swap tap and hold: Options > Control Options > Weapon Switch.", "Vous avez maintenant tous les types d'armes. Maintenez un bouton d'arme (croix gauche, droite ou bas ; touches 1, 2 et 3 ; ou le bouton d'armes à l'écran) pour équiper aussitôt l'arme choisie de ce type. Un appui ouvre le menu des armes pour en choisir une autre. Pour inverser : Options > Commandes > Changement d'arme.", "Du hast jetzt jede Waffenart. Halte eine Waffentaste (Steuerkreuz links, rechts oder unten; Tasten 1, 2 und 3; oder die Waffentaste auf dem Bildschirm), um sofort die gewählte Waffe dieser Art auszurüsten. Ein Tippen öffnet das Waffenmenü, um eine andere zu wählen. Zum Tauschen: Optionen > Steuerung > Waffenwechsel."},
    {9006470, "Press A or click to continue.", "Appuyez sur A ou cliquez pour continuer.", "A drücken oder klicken, um fortzufahren."},

    // Language.
    {9003000, "  (in use)", "  (actuelle)", "  (aktiv)"},
    {9003010, "Changes the language of the game text, right now.", "Change la langue du texte du jeu, tout de suite.",
     "Ändert die Sprache des Spieltexts sofort."},

    // Achievements.
    {9004000, "Achievements", "Succès", "Erfolge"},
    {9004010, "Unlocked", "Débloqués", "Freigeschaltet"},
    {9004020, "Points", "Points", "Punkte"},
    {9004030, "Locked", "Verrouillé", "Gesperrt"},
    {9004040, "Achievement unlocked", "Succès débloqué", "Erfolg freigeschaltet"},
    {9004050, "Achievements show once the game has loaded.", "Les succès apparaissent une fois le jeu chargé.",
     "Erfolge erscheinen, sobald das Spiel geladen ist."},
    {9004060, "See your achievements.", "Consulter vos succès.", "Erfolge ansehen."},
    {9004070, "pts", "pts", "Pkt."},
    {9004080, "unlocked on", "débloqué le", "freigeschaltet am"},
    {9004090, "Next Page", "Page suivante", "Nächste Seite"},
    {9004100, "Previous Page", "Page précédente", "Vorherige Seite"},
    {9004110, "Shows the other achievements.", "Affiche les autres succès.", "Zeigt die anderen Erfolge."},
    {9004120, "Achievements unlocked on this PC, in every save.", "Succès débloqués sur ce PC, dans toutes les sauvegardes.",
     "Auf diesem PC freigeschaltete Erfolge, in jedem Spielstand."},
    // Nier Plus (2026-11-15): the code mods' settings in the menus (mods.cpp).
    {9004200, "Mods", "Mods", "Mods"},
    {9004210, "Settings of the installed mods.", "Réglages des mods installés.", "Einstellungen der installierten Mods."},
    {9004220, "Changes this mod's setting.", "Modifie ce réglage du mod.", "Ändert diese Einstellung des Mods."},
    {9004230, "Runs this mod's action.", "Exécute cette action du mod.", "Führt diese Aktion des Mods aus."},
    {9004240, "Opens this mod's settings.", "Ouvre les réglages de ce mod.", "Öffnet die Einstellungen dieses Mods."},
    {9004250, "(off)", "(désactivé)", "(aus)"},
    {9004260, "Mod Active", "Mod actif", "Mod aktiv"},
    {9004270, "Turns this mod on or off. Off, it does nothing.", "Active ou désactive ce mod. Désactivé, il ne fait rien.",
     "Schaltet diesen Mod ein oder aus. Aus tut er nichts."},
    {9004280, "Shortcut", "Raccourci", "Kürzel"},
    {9004290, "Key that opens this mod's window. Press A, then the new key. Esc cancels.",
     "Touche qui ouvre la fenêtre de ce mod. Appuyez sur A, puis sur la nouvelle touche. Échap annule.",
     "Taste, die das Fenster dieses Mods öffnet. A drücken, dann die neue Taste. Esc bricht ab."},
    {9004300, "Back to Mods", "Retour aux mods", "Zurück zu den Mods"},
    {9004310, "Back to the list of installed mods.", "Retour à la liste des mods installés.",
     "Zurück zur Liste der installierten Mods."},
};

// One table read from a PC_TEXT.TXD: id -> its English column.
using Table = std::map<uint32_t, std::string>;

Table ReadTable(const fs::path& file) {
  Table table;
  std::ifstream in(file, std::ios::binary);
  if (!in) return table;
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
  size_t at = 0;
  while (at < data.size()) {
    size_t end = data.find("\r\n", at);
    if (end == std::string::npos) end = data.size();
    const std::string line = data.substr(at, end - at);
    at = end + 2;
    std::vector<std::string> cols;
    size_t c = 0;
    while (true) {
      const size_t tab = line.find('\t', c);
      cols.push_back(line.substr(c, tab == std::string::npos ? std::string::npos : tab - c));
      if (tab == std::string::npos) break;
      c = tab + 1;
    }
    if (cols.size() < 3 || cols[0].empty() || cols[0][0] < '0' || cols[0][0] > '9') continue;
    table[uint32_t(std::stoul(cols[0]))] = cols[2];
  }
  return table;
}

struct State {
  std::mutex mutex;
  fs::path dir;  // translation folder the table was read from
  Table table;
};
State& S() {
  static State s;
  return s;
}

// The active translation's table (re-read when the language changes).
const Table& TranslationLocked(State& s) {
  const fs::path dir = nier::text::CurrentDir();
  if (dir != s.dir) {
    s.dir = dir;
    s.table = dir.empty() ? Table() : ReadTable(dir / "PC_TEXT.TXD");
    if (!dir.empty()) REXLOG_INFO("Textos do PC: {} linhas em {}", s.table.size(), (dir / "PC_TEXT.TXD").string());
  }
  return s.table;
}

std::string Escape(std::string s) {  // a cell holds no TAB and no CRLF
  for (char& c : s) {
    if (c == '\t' || c == '\r') c = ' ';
  }
  return s;
}

}  // namespace

std::string T(const char* en) {
  const Entry* entry = nullptr;
  for (const Entry& e : kEntries) {
    if (std::strcmp(e.en, en) == 0) {
      entry = &e;
      break;
    }
  }
  if (!entry) return en;
  {
    State& s = S();
    std::lock_guard lock(s.mutex);
    const Table& t = TranslationLocked(s);
    if (auto it = t.find(entry->id); it != t.end() && !it->second.empty()) return it->second;
  }
  switch (nier::text::GameLanguageOverride()) {
    case 3: return entry->fr;
    case 4: return entry->de;
    default: return entry->en;
  }
}

std::string Achievement(uint32_t id, int field, const std::string& fallback) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  const Table& t = TranslationLocked(s);
  if (auto it = t.find(9500000 + id * 10 + uint32_t(field)); it != t.end() && !it->second.empty()) return it->second;
  return fallback;
}

void WriteExample() {
  const fs::path root = nier::text::Root();
  if (root.empty()) return;
  std::ostringstream out;
  out << "\xEF\xBB\xBF";
  for (const Entry& e : kEntries) {
    out << e.id << '\t' << e.en << '\t' << e.en << '\t' << e.fr << '\t' << e.de << "\r\n";
  }
  // The achievements in the game's own words (its XDBF), English in the Japanese column too.
  const auto en = nier::achievements::List(2), fr = nier::achievements::List(3), de = nier::achievements::List(4);
  for (size_t i = 0; i < en.size(); ++i) {
    auto field = [](const nier::achievements::Info& a, int f) {
      return Escape(f == 0 ? a.name : f == 1 ? a.description : a.locked);
    };
    for (int f = 0; f < 3; ++f) {
      const std::string e = field(en[i], f);
      const std::string fr_text = i < fr.size() ? field(fr[i], f) : e, de_text = i < de.size() ? field(de[i], f) : e;
      out << (9500000 + en[i].id * 10 + uint32_t(f)) << '\t' << e << '\t' << e << '\t' << fr_text << '\t' << de_text
          << "\r\n";
    }
  }
  const std::string data = out.str();
  std::error_code ec;
  const fs::path dir = root / "EXAMPLE";
  if (!fs::is_directory(dir, ec)) return;  // made by ExtractExample, later
  const fs::path file = dir / "PC_TEXT.TXD";
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (old == data) return;
    }
  }
  std::ofstream o(file, std::ios::binary);
  o.write(data.data(), std::streamsize(data.size()));
  REXLOG_INFO("Textos do PC: {} escrito", file.string());
}

}  // namespace nier::pc_text
