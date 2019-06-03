//
// Created by zmx on 2019-05-31.
//

#include <eosiolib/eosio.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/crypto.h>

using namespace eosio;

class dice: contract {
private:
    // @abi table account
    struct account {
        account_name owner;
        asset eos_balance;
        uint64_t open_offers = 0;
        uint64_t open_games = 0;

        uint64_t primary_key() const { return owner; }
    };

    typedef multi_index<N(account), account> account_index;
    account_index accounts;

    // @abi table offer
    struct offer {
        uint64_t id;
        account_name owner;
        asset bet;
        checksum256 commitment;
        uint64_t game_id = 0;

        uint64_t primary_key() const { return id; }
        uint64_t by_bet() const { return (uint64_t)bet.amount; }
        key256 by_commitment() const { return get_commitment(commitment); }

        static key256 get_commitment(const checksum256& commitment) {
            const uint64_t *p64 = reinterpret_cast<const uint64_t *>(&commitment);
            return key256::make_from_word_sequence<uint64_t>(p64[0], p64[1], p64[2], p64[3]);
        }
    };

    typedef multi_index<N(offer), offer, indexed_by<N(bet), const_mem_fun<offer, uint64_t, &offer::by_bet>>, indexed_by<N(commitment), const_mem_fun<offer, key256, &offer::by_commitment>>> offer_index;
    offer_index offers;

    struct player {
        checksum256 commitment;
        checksum256 reveal;
    };

    // @abi table game
    struct game {
        uint64_t id;
        asset bet;
        uint64_t deadline;
        player player1;
        player player2;

        uint64_t primary_key() const { return id; }
    };

    typedef multi_index<N(game), game> game_index;
    game_index games;

    // @abi table global
    struct global_dice {
        uint64_t id;
        uint64_t next_game_id;

        uint64_t primary_key() const { return id; }
    };

    typedef multi_index<N(global), global_dice> global_dice_index;
    global_dice_index global_dices;

    bool has_offer(const checksum256& commitment) const {
        auto idx = offers.template get_index<N(commitment)>();
        auto iter = idx.find(offer::get_commitment(commitment));
        return iter != idx.end();
    }

    bool is_equal(const checksum256& x, const checksum256& y) const {
        return memcmp((void *)&x, (void *)&y, sizeof(x)) == 0;
    }

    bool is_zero(const checksum256& commitment) const {
        const uint64_t *p64 = reinterpret_cast<const uint64_t *>(&commitment);
        return p64[0] == 0 && p64[1] == 0 && p64[2] == 0 && p64[3] == 0;
    }

    void pay(const game& game, const offer& winner_offer, const offer& loser_offer) {
        accounts.modify(accounts.find(winner_offer.owner), 0, [&](auto& r) {
            r.eos_balance += 2*game.bet;
            r.open_games--;
        });

        accounts.modify(accounts.find(loser_offer.owner), 0, [&](auto& r) {
            r.open_games--;
        });
    }

    void validate_asset(const asset a) const {
        eosio_assert(a.is_valid(), "invalid quantity");
        eosio_assert(a.amount > 0, "quantity should be above 0");
        eosio_assert(a.symbol == S(4, EOS), "should be eos");
    }

public:
    dice(account_name self): contract(self), accounts(self, self), offers(self, self), games(self, self), global_dices(self, self) {}

    // @abi action
    void deposit(const account_name from, const asset quantity) {
        validate_asset(quantity);
        require_auth(from);

        auto iter = accounts.find(from);
        if(iter == accounts.end()) {
            iter = accounts.emplace(_self, [&](auto& r) {
                r.eos_balance = quantity;
                r.owner = from;
            });
        } else {
            accounts.modify(iter, 0, [&](auto& r) {
                r.eos_balance += quantity;
            });
        }

        action(
                permission_level{from, N(active)},
                N(eosio.token), N(transfer),
                std::make_tuple(from, _self, quantity, std::string(""))
                ).send();
    }

    // @abi action
    void withdraw(const account_name from, const asset quantity) {
        validate_asset(quantity);
        require_auth(from);

        auto iter = accounts.find(from);
        eosio_assert(iter != accounts.end(), "account doesn't exist");

        accounts.modify(iter, 0, [&](auto& r) {
            eosio_assert(r.eos_balance >= quantity, "insufficient balance");
            r.eos_balance -= quantity;
        });

        action(
                permission_level(_self, N(active)),
                N(eosio.token), N(transfer),
                std::make_tuple(_self, from, quantity, std::string(""))
                ).send();
    }

    // @abi action
    void offerbet(const asset bet, const account_name player, const checksum256& commitment) {
        validate_asset(bet);
        eosio_assert(!has_offer(commitment), "offer exist");
        require_auth(player);

        auto account_iter = accounts.find(player);
        eosio_assert(account_iter != accounts.end(), "account doesn't exist");

        auto new_offer_iter = offers.emplace(_self, [&](auto& r) {
            r.id = offers.available_primary_key();
            r.owner = player;
            r.bet = bet;
            r.commitment = commitment;
        });

        auto idx = offers.template get_index<N(bet)>();
        auto match_offer_iter = idx.lower_bound((uint64_t)new_offer_iter->bet.amount);
        if(match_offer_iter == idx.end() || match_offer_iter->bet != new_offer_iter->bet || match_offer_iter->owner == new_offer_iter->owner) {
            accounts.modify(account_iter, 0, [&](auto& r) {
                eosio_assert(r.eos_balance >= bet, "insufficient balance");
                r.eos_balance -= bet;
                r.open_offers++;
            });
        } else {
            auto global_dice_iter = global_dices.begin();
            if(global_dice_iter == global_dices.end()) {
                global_dice_iter = global_dices.emplace(_self, [&](auto& r) {
                    r.id = 0;
                    r.next_game_id = 0;
                });
            }

            global_dices.modify(global_dice_iter, 0, [&](auto& r) {
                r.next_game_id++;
            });

            auto game_iter = games.emplace(_self, [&](auto& r) {
                r.id = global_dice_iter->next_game_id;
                r.bet = bet;
                r.deadline = 0;

                r.player1.commitment = match_offer_iter->commitment;
                memset(&r.player1.reveal, 0, sizeof(checksum256));

                r.player2.commitment = new_offer_iter->commitment;
                memset(&r.player2.reveal, 0, sizeof(checksum256));
            });

            idx.modify(match_offer_iter, 0, [&](auto& r) {
                r.bet.amount = 0;
                r.game_id = game_iter->id;
            });

            offers.modify(new_offer_iter, 0, [&](auto& r) {
                r.bet.amount = 0;
                r.game_id = game_iter->id;
            });

            accounts.modify(accounts.find(match_offer_iter->owner), 0, [&](auto& r) {
                r.open_offers--;
                r.open_games++;
            });

            accounts.modify(account_iter, 0, [&](auto& r) {
                eosio_assert(r.eos_balance >= bet, "insufficient balance");
                r.eos_balance -= bet;
                r.open_games++;
            });
        }
    }

    // @abi action
    void canceloffer(const checksum256& commitment) {
        auto idx = offers.template get_index<N(commitment)>();
        auto offer_iter = idx.find(offer::get_commitment(commitment));
        eosio_assert(offer_iter != idx.end(), "offer doesn't exist");
        eosio_assert(offer_iter->game_id == 0, "offer can't be canceled");
        require_auth(offer_iter->owner);
        accounts.modify(accounts.find(offer_iter->owner), 0, [&](auto& r) {
            r.open_offers--;
            r.eos_balance += offer_iter->bet;
        });
        idx.erase(offer_iter);
    }

    // @abi action
    void reveal(const checksum256& commitment, const checksum256& source) {
        assert_sha256((char *)&source, sizeof(source), (checksum256 *)&commitment);

        auto idx = offers.template get_index<N(commitment)>();
        auto iter = idx.find(offer::get_commitment(commitment));

        eosio_assert(iter != idx.end(), "offer doesn't exist");
        eosio_assert(iter->game_id > 0, "offer can't be revealed");
        require_auth(iter->owner);
        auto game_iter = games.find(iter->game_id);
        player player1 = game_iter->player1;
        player player2 = game_iter->player2;
        if(!is_equal(commitment, player2.commitment)) {
            std::swap(player1, player2);
        }

        eosio_assert(is_zero(player2.reveal), "offer has been revealed");
        games.modify(game_iter, 0, [&](auto& r) {
            if(is_equal(commitment, r.player1.commitment)) {
                r.player1.reveal = source;
            } else {
                r.player2.reveal = source;
            }
            if(is_zero(player1.reveal)) {
                r.deadline = now()+60;
            }
        });

        if(!is_zero(player1.reveal)) {
            checksum256 result;
            sha256((char *)&game_iter->player1, 2*sizeof(player), &result);
            auto prev_offer_iter = idx.find(offer::get_commitment(player1.commitment));
            if(result.hash[0] > result.hash[1]) {
                pay(*game_iter, *prev_offer_iter, *iter);
            } else {
                pay(*game_iter, *iter, *prev_offer_iter);
            }
        }
    }

    // @abi action
    void expire(const uint64_t game_id) {
        auto game_iter = games.find(game_id);
        eosio_assert(game_iter != games.end(), "game doesn't exist");
        eosio_assert(game_iter->deadline > 0 && now() > game_iter->deadline, "game isn't expired");
        eosio_assert(is_zero(game_iter->player1.reveal) || is_zero(game_iter->player2.reveal), "both have revealed");
        auto idx = offers.template get_index<N(commitment)>();
        auto offer_iter1 = idx.find(offer::get_commitment(game_iter->player1.commitment));
        auto offer_iter2 = idx.find(offer::get_commitment(game_iter->player2.commitment));
        if(is_zero(game_iter->player1.reveal)) {
            pay(*game_iter, *offer_iter2, *offer_iter1);
        } else {
            pay(*game_iter, *offer_iter1, *offer_iter2);
        }
    }
};

EOSIO_ABI(dice, (deposit)(withdraw)(offerbet)(canceloffer)(reveal)(expire))