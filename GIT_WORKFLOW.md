# Far Cry 2 Coop Mod - Git Workflow Pro Teamovou Práci

## 🌐 Remote Repository
**GitHub:** https://github.com/precomp666/fc2-coop-mod.git

---

## 🚀 Nastavení pro VŠECHNY pracovní stanice (PC)

### První nastavení na NOVÉM PC:

```bash
# 1. Stáhnete repo z GitHubu
git clone https://github.com/precomp666/fc2-coop-mod.git FC2-Coop-Project
cd FC2-Coop-Project

# 2. Nastavíte své jméno a email (NEZAPOMÍŇTE!)
git config --global user.name "VÁŠ_NÁZEV"
git config --global user.email "váš@email.com"

# 3. Spuštění práce!
```

---

## 🔄 Denní Workflow pro KAŽDÉHO členu týmu:

### 1. Aktualizace z hlavního repozitáře:
```bash
cd ~/Downloads/Far\ Cry\ 2
git fetch origin          # Stáhnout nová data z GitHubu
git pull origin master    # Vytahat změny do lokálního repozitáře
```

### 2. Vývoj nových funkcí:
```bash
# Vytvoření VLASTNÍ branch pro vaši práci
git checkout -b feature-vaše-nazev-funkce

# Práce na kódu...

git add .                # Přidat změny
git commit -m "popis změn"  # Udělat commit
git push origin feature-vaše-nazev-funkce  # Poslat do GitHubu
```

### 3. Spojení s hlavní branch:
```bash
# Po dokončení funkce, spojte se s masterem
git checkout master
git pull origin master
git merge feature-vaše-nazev-funkce    # Mergovat svůj commit
git push origin master                  # Pushnout merge
```

---

## ⚠️ DŮLEŽITÁ PRAVIDLA PRO TEAMOVOU PRÁCI:

| Do | Co udělat |
|-----|-----------|
| ✅ Vždy | `git pull` před začátkem práce |
| ✅ Vždy | Práce na VLASTNÍ branch (`feature-xxx`) |
| ✅ Vždy | Commitovat s popisnými zprávami |
| ❌ Nikdy | Přímý edit `master` bez pullu |
| ❌ Nikdy | Pushnout bez pullu nejprve! |

---

## 🎮 Struktura projektu:

```
├── bin/              # Compilovaný kód
├── coop_dev/        # Coop vývojové soubory
├── Data_Win32/       # Hrávací data
├── installers/       # Instalátory
└── fc2_coop.log      # Log soubor
```

---

## 🤝 Spolupráce v týmu:

1. **Člen A** vytváří branch: `git checkout -b feature-ai-systém`
2. **Člen B** pracuje na jiném kódu v jiné branchi
3. Oběma pushují do GitHubu
4. Člen A merge své změny do master a push
5. Všichni mají vždy aktuální verzi: `git pull origin master`

---

## 🔐 Bezpečnostní tipy:

- Nikdy necommitujte `.gitignore` obsah s citlivými daty!
- Backup: Každý commit = automatický backup na GitHubu!
- Pokud je něco špatně: `git reset --hard HEAD~1` (odeberat posledního commit)

---

## 📝 Příklad commit zpráv:

```
feat: přidán AI systém pro coop NPC
fix: opravena chyba v movement code
docs: aktualizace ReadMe s instrukcemi
refactor: přepracování memory managementu
```

---

## 🎉 Výhody Git synchronizace:

✅ **Automatický backup** - GitHub ukládá každý commit!  
✅ **Práce na různých PCch** - stačí `git clone` nebo `pull`  
✅ **Verze historie** - vidět všechny změny  
✅ **Žádné konflikty dat** - Git řeší kolize automaticky  
✅ **Team workflow** - každý pracuje na své branchi  

---

*Poslední aktualizace: 2026-10-08*