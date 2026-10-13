# Clé de signature

`signing_key.pem` (RSA-3072) signe chaque image. Elle n'est **jamais** commitée.

**Sauvegarde-la hors du PC** (gestionnaire de mots de passe, clé USB). Sans elle, plus aucune OTA
n'est possible : il faudrait rebrancher l'USB.

Génération (une seule fois) :

    idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem
